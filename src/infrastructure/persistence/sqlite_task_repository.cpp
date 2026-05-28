#include <cmlb/infrastructure/persistence/sqlite_task_repository.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <sqlite_modern_cpp.h>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>

#include "time_codec.hpp"

namespace cmlb::infrastructure::persistence {

namespace {

using detail::parse_iso8601;
using detail::to_iso8601;
using SysTime = detail::SysTime;

[[nodiscard]] std::string_view kind_to_string(domain::TaskKind k) noexcept {
    switch (k) {
        case domain::TaskKind::Mirror: return "Mirror";
        case domain::TaskKind::Leech:  return "Leech";
        case domain::TaskKind::Clone:  return "Clone";
    }
    return "Unknown";
}

[[nodiscard]] core::Result<domain::TaskKind> parse_kind(std::string_view s) {
    if (s == "Mirror") return domain::TaskKind::Mirror;
    if (s == "Leech")  return domain::TaskKind::Leech;
    if (s == "Clone")  return domain::TaskKind::Clone;
    return core::error(core::ErrorCode::Deserialization,
                       "Unknown TaskKind: " + std::string{s});
}

[[nodiscard]] core::Result<domain::DownloaderKind>
parse_downloader_kind(std::string_view s) {
    if (s == "None")        return domain::DownloaderKind::None;
    if (s == "Aria2")       return domain::DownloaderKind::Aria2;
    if (s == "Qbittorrent") return domain::DownloaderKind::Qbittorrent;
    return core::error(core::ErrorCode::Deserialization,
                       "Unknown DownloaderKind: " + std::string{s});
}

[[nodiscard]] core::Result<domain::TaskState> parse_state(std::string_view s) {
    if (s == "Queued")      return domain::TaskState::Queued;
    if (s == "Downloading") return domain::TaskState::Downloading;
    if (s == "Processing")  return domain::TaskState::Processing;
    if (s == "Uploading")   return domain::TaskState::Uploading;
    if (s == "Completed")   return domain::TaskState::Completed;
    if (s == "Failed")      return domain::TaskState::Failed;
    if (s == "Cancelled")   return domain::TaskState::Cancelled;
    return core::error(core::ErrorCode::Deserialization,
                       "Unknown TaskState: " + std::string{s});
}

/// Replays the minimum number of state transitions required to drive a freshly
/// constructed Task to @p target. Returns the populated Task.
[[nodiscard]] core::Result<domain::Task>
reconstitute(domain::TaskMetadata metadata,
             domain::TaskState target,
             std::optional<std::string> error_message,
             domain::DownloaderKind downloader_kind,
             std::optional<std::string> downloader_id) {
    domain::Task task{std::move(metadata)};
    auto fail = [](std::string_view step, const core::AppError& err) {
        return core::error(core::ErrorCode::Deserialization,
                           std::string{"reconstitute step '"} + std::string{step}
                               + "' failed: " + err.message);
    };

    auto attach_downloader_if_set = [&]() {
        if (downloader_kind != domain::DownloaderKind::None && downloader_id) {
            task.attach_downloader(domain::Gid{std::move(*downloader_id)},
                                   downloader_kind);
        }
    };

    switch (target) {
        case domain::TaskState::Queued:
            attach_downloader_if_set();
            return task;

        case domain::TaskState::Downloading: {
            if (auto r = task.start_download(); !r) return fail("start_download", r.error());
            attach_downloader_if_set();
            return task;
        }
        case domain::TaskState::Processing: {
            if (auto r = task.start_download(); !r) return fail("start_download", r.error());
            if (auto r = task.begin_processing(); !r) return fail("begin_processing", r.error());
            attach_downloader_if_set();
            return task;
        }
        case domain::TaskState::Uploading: {
            if (auto r = task.start_download(); !r) return fail("start_download", r.error());
            if (auto r = task.begin_upload(); !r) return fail("begin_upload", r.error());
            attach_downloader_if_set();
            return task;
        }
        case domain::TaskState::Completed: {
            if (auto r = task.start_download(); !r) return fail("start_download", r.error());
            if (auto r = task.begin_upload(); !r) return fail("begin_upload", r.error());
            if (auto r = task.mark_completed(); !r) return fail("mark_completed", r.error());
            attach_downloader_if_set();
            return task;
        }
        case domain::TaskState::Failed: {
            attach_downloader_if_set();
            const std::string reason = error_message.value_or("(unknown)");
            if (auto r = task.mark_failed(reason); !r) return fail("mark_failed", r.error());
            return task;
        }
        case domain::TaskState::Cancelled: {
            attach_downloader_if_set();
            if (auto r = task.mark_cancelled(); !r) return fail("mark_cancelled", r.error());
            return task;
        }
    }
    return core::error(core::ErrorCode::Deserialization,
                       "reconstitute: unrecognised TaskState enumerator");
}

}  // namespace

boost::asio::awaitable<core::Result<void>>
SqliteTaskRepository::save(domain::Task task) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    const auto& md            = task.metadata();
    const std::string id      = md.id.value();
    const std::int64_t user   = md.user.value();
    const std::int64_t chat   = md.chat.value();
    const std::int64_t msg    = md.status_message.value();
    const std::string kind    = std::string{kind_to_string(md.kind)};
    const std::string state   = std::string{domain::to_string(task.state())};
    const std::string created = to_iso8601(md.created_at);
    const std::string updated = to_iso8601(md.updated_at);

    const std::string downloader_kind{domain::to_string(task.downloader_kind())};
    std::optional<std::string> downloader_id;
    if (const auto did = task.downloader_id(); did.has_value()) {
        downloader_id = did->value();
    }

    std::optional<std::string> err_msg;
    if (const auto e = task.error_message(); e.has_value()) {
        err_msg = std::string{*e};
    }

    // sqlite-modern-cpp does not natively bind std::optional<std::string>, so
    // the four combinations of (error_message, downloader_id) being NULL or set
    // each get their own INSERT. The cross-product is small enough to spell out
    // and keeps the wire-level statement explicit.
    try {
        constexpr std::string_view kColumns =
            "(id, user_id, chat_id, status_message_id, kind, state, "
            "source_url, error_message, created_at, updated_at, "
            "downloader_kind, downloader_id)";

        const bool has_err = err_msg.has_value();
        const bool has_did = downloader_id.has_value();

        if (has_err && has_did) {
            db << "INSERT OR REPLACE INTO tasks " + std::string{kColumns} +
                  " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
               << id << user << chat << msg << kind << state
               << md.source_url << *err_msg << created << updated
               << downloader_kind << *downloader_id;
        } else if (has_err) {
            db << "INSERT OR REPLACE INTO tasks " + std::string{kColumns} +
                  " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL);"
               << id << user << chat << msg << kind << state
               << md.source_url << *err_msg << created << updated
               << downloader_kind;
        } else if (has_did) {
            db << "INSERT OR REPLACE INTO tasks " + std::string{kColumns} +
                  " VALUES (?, ?, ?, ?, ?, ?, ?, NULL, ?, ?, ?, ?);"
               << id << user << chat << msg << kind << state
               << md.source_url << created << updated
               << downloader_kind << *downloader_id;
        } else {
            db << "INSERT OR REPLACE INTO tasks " + std::string{kColumns} +
                  " VALUES (?, ?, ?, ?, ?, ?, ?, NULL, ?, ?, ?, NULL);"
               << id << user << chat << msg << kind << state
               << md.source_url << created << updated
               << downloader_kind;
        }
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"save(Task) failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"save(Task) threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<std::optional<domain::Task>>>
SqliteTaskRepository::find(domain::TaskId id) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        std::optional<domain::Task> result;
        core::Result<domain::Task>  parse_outcome = core::error(
            core::ErrorCode::None, "uninit");  // overwritten before use
        bool                        any_row = false;

        db << R"SQL(
            SELECT user_id, chat_id, status_message_id, kind, state, source_url,
                   error_message, created_at, updated_at,
                   downloader_kind, downloader_id
              FROM tasks
             WHERE id = ?;
        )SQL"
           << id.value()
           >> [&](std::int64_t                   user_id,
                  std::int64_t                   chat_id,
                  std::int64_t                   status_msg,
                  std::string                    kind_str,
                  std::string                    state_str,
                  std::string                    source_url,
                  std::unique_ptr<std::string>   err_msg,
                  std::string                    created_at,
                  std::string                    updated_at,
                  std::string                    downloader_kind_str,
                  std::unique_ptr<std::string>   downloader_id_str) {
               any_row = true;

               auto kind_parsed       = parse_kind(kind_str);
               auto state_parsed      = parse_state(state_str);
               auto created_tp        = parse_iso8601(created_at);
               auto updated_tp        = parse_iso8601(updated_at);
               auto downloader_parsed = parse_downloader_kind(downloader_kind_str);

               if (!kind_parsed)       { parse_outcome = std::unexpected{kind_parsed.error()};       return; }
               if (!state_parsed)      { parse_outcome = std::unexpected{state_parsed.error()};      return; }
               if (!created_tp)        { parse_outcome = std::unexpected{created_tp.error()};        return; }
               if (!updated_tp)        { parse_outcome = std::unexpected{updated_tp.error()};        return; }
               if (!downloader_parsed) { parse_outcome = std::unexpected{downloader_parsed.error()}; return; }

               domain::TaskMetadata md{
                   .id             = id,
                   .user           = domain::UserId{user_id},
                   .chat           = domain::ChatId{chat_id},
                   .status_message = domain::MessageId{status_msg},
                   .kind           = *kind_parsed,
                   .source_url     = std::move(source_url),
                   .created_at     = *created_tp,
                   .updated_at     = *updated_tp,
               };

               std::optional<std::string> err_opt;
               if (err_msg) {
                   err_opt = std::move(*err_msg);
               }

               std::optional<std::string> did_opt;
               if (downloader_id_str) {
                   did_opt = std::move(*downloader_id_str);
               }

               parse_outcome = reconstitute(
                   std::move(md), *state_parsed, std::move(err_opt),
                   *downloader_parsed, std::move(did_opt));
           };

        if (!any_row) {
            co_return std::optional<domain::Task>{std::nullopt};
        }
        if (!parse_outcome.has_value()) {
            co_return std::unexpected{parse_outcome.error()};
        }
        result = std::move(*parse_outcome);
        co_return result;
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"find(TaskId) failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"find(TaskId) threw: "} + ex.what());
    }
}

namespace {

[[nodiscard]] core::Result<std::vector<domain::Task>>
collect_tasks(sqlite::database& db, std::string_view sql,
              std::optional<std::int64_t> bound_user) {
    std::vector<domain::Task> out;
    core::Result<void>        loop_status{};

    auto consume = [&](std::string                  id,
                       std::int64_t                 user_id,
                       std::int64_t                 chat_id,
                       std::int64_t                 status_msg,
                       std::string                  kind_str,
                       std::string                  state_str,
                       std::string                  source_url,
                       std::unique_ptr<std::string> err_msg,
                       std::string                  created_at,
                       std::string                  updated_at,
                       std::string                  downloader_kind_str,
                       std::unique_ptr<std::string> downloader_id_str) {
        if (!loop_status.has_value()) {
            return;  // stop accumulating after first error.
        }
        auto kind_parsed       = parse_kind(kind_str);
        auto state_parsed      = parse_state(state_str);
        auto created_tp        = parse_iso8601(created_at);
        auto updated_tp        = parse_iso8601(updated_at);
        auto downloader_parsed = parse_downloader_kind(downloader_kind_str);

        if (!kind_parsed)       { loop_status = std::unexpected{kind_parsed.error()};       return; }
        if (!state_parsed)      { loop_status = std::unexpected{state_parsed.error()};      return; }
        if (!created_tp)        { loop_status = std::unexpected{created_tp.error()};        return; }
        if (!updated_tp)        { loop_status = std::unexpected{updated_tp.error()};        return; }
        if (!downloader_parsed) { loop_status = std::unexpected{downloader_parsed.error()}; return; }

        domain::TaskMetadata md{
            .id             = domain::TaskId{std::move(id)},
            .user           = domain::UserId{user_id},
            .chat           = domain::ChatId{chat_id},
            .status_message = domain::MessageId{status_msg},
            .kind           = *kind_parsed,
            .source_url     = std::move(source_url),
            .created_at     = *created_tp,
            .updated_at     = *updated_tp,
        };

        std::optional<std::string> err_opt;
        if (err_msg) {
            err_opt = std::move(*err_msg);
        }

        std::optional<std::string> did_opt;
        if (downloader_id_str) {
            did_opt = std::move(*downloader_id_str);
        }

        auto task_or_err = reconstitute(std::move(md), *state_parsed,
                                        std::move(err_opt),
                                        *downloader_parsed,
                                        std::move(did_opt));
        if (!task_or_err.has_value()) {
            loop_status = std::unexpected{task_or_err.error()};
            return;
        }
        out.push_back(std::move(*task_or_err));
    };

    if (bound_user.has_value()) {
        db << std::string{sql} << *bound_user >> consume;
    } else {
        db << std::string{sql} >> consume;
    }

    if (!loop_status.has_value()) {
        return std::unexpected{loop_status.error()};
    }
    return out;
}

}  // namespace

boost::asio::awaitable<core::Result<std::vector<domain::Task>>>
SqliteTaskRepository::incomplete() {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        constexpr std::string_view kSql = R"SQL(
            SELECT id, user_id, chat_id, status_message_id, kind, state,
                   source_url, error_message, created_at, updated_at,
                   downloader_kind, downloader_id
              FROM tasks
             WHERE state NOT IN ('Completed', 'Failed', 'Cancelled')
             ORDER BY created_at ASC;
        )SQL";
        co_return collect_tasks(db, kSql, std::nullopt);
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"incomplete() failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"incomplete() threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<std::vector<domain::Task>>>
SqliteTaskRepository::for_user(domain::UserId user) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        constexpr std::string_view kSql = R"SQL(
            SELECT id, user_id, chat_id, status_message_id, kind, state,
                   source_url, error_message, created_at, updated_at,
                   downloader_kind, downloader_id
              FROM tasks
             WHERE user_id = ?
             ORDER BY created_at DESC;
        )SQL";
        co_return collect_tasks(db, kSql, user.value());
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"for_user() failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"for_user() threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<void>>
SqliteTaskRepository::remove(domain::TaskId id) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        db << "DELETE FROM tasks WHERE id = ?;" << id.value();
        const int changes = sqlite3_changes(db.connection().get());
        if (changes == 0) {
            co_return core::error(core::ErrorCode::NotFound,
                                  "remove(TaskId): no task with id " + id.value());
        }
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"remove(TaskId) failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"remove(TaskId) threw: "} + ex.what());
    }
}

}  // namespace cmlb::infrastructure::persistence
