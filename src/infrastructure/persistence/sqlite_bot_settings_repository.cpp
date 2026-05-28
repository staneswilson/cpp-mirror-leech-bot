#include <cmlb/infrastructure/persistence/sqlite_bot_settings_repository.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <nlohmann/json.hpp>
#include <sqlite_modern_cpp.h>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>

#include "time_codec.hpp"

namespace cmlb::infrastructure::persistence {

namespace {

using detail::parse_iso8601;
using detail::to_iso8601;

[[nodiscard]] std::string encode_id_array(const std::vector<std::int64_t>& ids) {
    return nlohmann::json(ids).dump();
}

[[nodiscard]] core::Result<std::vector<std::int64_t>> decode_id_array(const std::string& json) {
    try {
        auto parsed = nlohmann::json::parse(json);
        if (!parsed.is_array()) {
            return core::error(core::ErrorCode::Deserialization,
                               "bot_settings ID array is not a JSON array: " + json);
        }
        std::vector<std::int64_t> out;
        out.reserve(parsed.size());
        for (const auto& entry : parsed) {
            if (!entry.is_number_integer()) {
                return core::error(core::ErrorCode::Deserialization,
                                   "bot_settings ID array contains non-integer entry");
            }
            out.push_back(entry.get<std::int64_t>());
        }
        return out;
    } catch (const nlohmann::json::exception& ex) {
        return core::error(core::ErrorCode::JsonParse,
                           std::string{"bot_settings ID array parse failed: "} + ex.what());
    }
}

}  // namespace

boost::asio::awaitable<core::Result<BotSettingsRecord>>
SqliteBotSettingsRepository::load() {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        BotSettingsRecord       record{};
        core::Result<void>      parse_status{};
        bool                    any_row = false;

        db << R"SQL(
            SELECT owner_id, sudo_users, authorized_chats, download_dir,
                   leech_split_size, upload_limit_bytes,
                   status_update_interval_ms, rss_poll_interval_ms, updated_at
              FROM bot_settings
             WHERE id = 1;
        )SQL"
           >> [&](std::int64_t owner,
                  std::string  sudo_json,
                  std::string  chats_json,
                  std::string  dl_dir,
                  std::int64_t split_size,
                  std::int64_t up_limit,
                  std::int64_t status_ms,
                  std::int64_t rss_ms,
                  std::string  updated_at) {
               any_row = true;

               auto sudo   = decode_id_array(sudo_json);
               auto chats  = decode_id_array(chats_json);
               auto ts     = parse_iso8601(updated_at);

               if (!sudo)  { parse_status = std::unexpected{sudo.error()};  return; }
               if (!chats) { parse_status = std::unexpected{chats.error()}; return; }
               if (!ts)    { parse_status = std::unexpected{ts.error()};    return; }

               record.owner_id               = owner;
               record.sudo_users             = std::move(*sudo);
               record.authorized_chats       = std::move(*chats);
               record.download_dir           = std::filesystem::path{dl_dir};
               record.leech_split_size       = split_size;
               record.upload_limit_bytes     = up_limit;
               record.status_update_interval = std::chrono::milliseconds{status_ms};
               record.rss_poll_interval      = std::chrono::milliseconds{rss_ms};
               record.updated_at             = *ts;
           };

        if (!parse_status.has_value()) {
            co_return std::unexpected{parse_status.error()};
        }
        if (!any_row) {
            // Return defaults — caller may persist them via save().
            co_return record;
        }
        co_return record;
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"bot_settings.load failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"bot_settings.load threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<void>>
SqliteBotSettingsRepository::save(BotSettingsRecord record) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    record.updated_at = std::chrono::system_clock::now();

    try {
        db << R"SQL(
            INSERT OR REPLACE INTO bot_settings
                (id, owner_id, sudo_users, authorized_chats, download_dir,
                 leech_split_size, upload_limit_bytes,
                 status_update_interval_ms, rss_poll_interval_ms, updated_at)
            VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )SQL"
           << record.owner_id
           << encode_id_array(record.sudo_users)
           << encode_id_array(record.authorized_chats)
           << record.download_dir.string()
           << record.leech_split_size
           << record.upload_limit_bytes
           << record.status_update_interval.count()
           << record.rss_poll_interval.count()
           << to_iso8601(record.updated_at);
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"bot_settings.save failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"bot_settings.save threw: "} + ex.what());
    }
}

}  // namespace cmlb::infrastructure::persistence
