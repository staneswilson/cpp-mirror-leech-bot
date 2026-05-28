#pragma once

#include <mutex>
#include <utility>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>

namespace cmlb::test_support {

class InMemoryBotSettingsRepository final
    : public cmlb::infrastructure::persistence::BotSettingsRepository {
public:
    InMemoryBotSettingsRepository()  = default;
    ~InMemoryBotSettingsRepository() override = default;

    boost::asio::awaitable<cmlb::core::Result<
        cmlb::infrastructure::persistence::BotSettingsRecord>>
    load() override {
        std::lock_guard lk{mutex_};
        co_return current_;
    }

    boost::asio::awaitable<cmlb::core::Result<void>>
    save(cmlb::infrastructure::persistence::BotSettingsRecord record)
        override {
        std::lock_guard lk{mutex_};
        current_ = std::move(record);
        co_return cmlb::core::Result<void>{};
    }

    /// Test helper: peek at the latest persisted record.
    [[nodiscard]] cmlb::infrastructure::persistence::BotSettingsRecord
    snapshot() const {
        std::lock_guard lk{mutex_};
        return current_;
    }

private:
    mutable std::mutex mutex_;
    cmlb::infrastructure::persistence::BotSettingsRecord current_{};
};

}  // namespace cmlb::test_support
