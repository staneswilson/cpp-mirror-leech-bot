#include "time_codec.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>

#include <sqlite_modern_cpp.h>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/upload_destination.hpp>
#include <cmlb/infrastructure/persistence/sqlite_user_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>

namespace cmlb::infrastructure::persistence {

namespace {

using detail::parse_iso8601;
using detail::to_iso8601;

} // namespace

boost::asio::awaitable<core::Result<std::optional<UserSettingsRecord>>>
SqliteUserSettingsRepository::get(domain::UserId user) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        std::optional<UserSettingsRecord> result;
        core::Result<void> parse_status{};

        db << R"SQL(
            SELECT leech_destination, mirror_destination, default_thumb_path,
                   rclone_remote, gdrive_folder_id, upload_as_document,
                   created_at, updated_at
              FROM user_settings
             WHERE user_id = ?;
        )SQL"
           << user.value()
            >>
            [&](std::string leech,
                std::string mirror,
                std::unique_ptr<std::string> thumb,
                std::unique_ptr<std::string> rclone_remote,
                std::unique_ptr<std::string> gdrive_folder,
                int upload_as_doc,
                std::string created_at,
                std::string updated_at) {
                auto leech_parsed = domain::parse_upload_destination(leech);
                auto mirror_parsed = domain::parse_upload_destination(mirror);
                auto created_tp = parse_iso8601(created_at);
                auto updated_tp = parse_iso8601(updated_at);

                if (!leech_parsed) {
                    parse_status = std::unexpected{leech_parsed.error()};
                    return;
                }
                if (!mirror_parsed) {
                    parse_status = std::unexpected{mirror_parsed.error()};
                    return;
                }
                if (!created_tp) {
                    parse_status = std::unexpected{created_tp.error()};
                    return;
                }
                if (!updated_tp) {
                    parse_status = std::unexpected{updated_tp.error()};
                    return;
                }

                UserSettingsRecord rec{
                    .user_id = user,
                    .leech_destination = *leech_parsed,
                    .mirror_destination = *mirror_parsed,
                    .default_thumb_path = thumb ? std::optional<std::string>{*thumb} : std::nullopt,
                    .rclone_remote =
                        rclone_remote ? std::optional<std::string>{*rclone_remote} : std::nullopt,
                    .gdrive_folder_id =
                        gdrive_folder ? std::optional<std::string>{*gdrive_folder} : std::nullopt,
                    .upload_as_document = upload_as_doc != 0,
                    .created_at = *created_tp,
                    .updated_at = *updated_tp,
                };
                result = std::move(rec);
            };

        if (!parse_status.has_value()) {
            co_return std::unexpected{parse_status.error()};
        }
        co_return result;
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"user_settings.get failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"user_settings.get threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<void>> SqliteUserSettingsRepository::save(
    UserSettingsRecord record) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    const auto now = std::chrono::system_clock::now();
    if (record.created_at.time_since_epoch().count() == 0) {
        record.created_at = now;
    }
    record.updated_at = now;

    const std::string leech = std::string{to_string(record.leech_destination)};
    const std::string mirror = std::string{to_string(record.mirror_destination)};
    const std::string created = to_iso8601(record.created_at);
    const std::string updated = to_iso8601(record.updated_at);

    try {
        // Bind nullable fields by branching to keep the statement parameters
        // simple — sqlite-modern-cpp does not natively bind std::optional<>
        // text columns.
        auto bind_text = [](sqlite::database_binder& binder, const std::optional<std::string>& v) {
            if (v.has_value()) {
                binder << *v;
            } else {
                binder << nullptr;
            }
        };

        auto stmt = db << R"SQL(
            INSERT OR REPLACE INTO user_settings
                (user_id, leech_destination, mirror_destination,
                 default_thumb_path, rclone_remote, gdrive_folder_id,
                 upload_as_document, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
        )SQL";
        stmt << record.user_id.value() << leech << mirror;
        bind_text(stmt, record.default_thumb_path);
        bind_text(stmt, record.rclone_remote);
        bind_text(stmt, record.gdrive_folder_id);
        stmt << (record.upload_as_document ? 1 : 0) << created << updated;
        stmt.execute();
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"user_settings.save failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"user_settings.save threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<void>> SqliteUserSettingsRepository::remove(
    domain::UserId user) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        db << "DELETE FROM user_settings WHERE user_id = ?;" << user.value();
        const int changes = sqlite3_changes(db.connection().get());
        if (changes == 0) {
            co_return core::error(core::ErrorCode::NotFound,
                                  "user_settings.remove: no row for user_id "
                                      + std::to_string(user.value()));
        }
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"user_settings.remove failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"user_settings.remove threw: "} + ex.what());
    }
}

} // namespace cmlb::infrastructure::persistence
