#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>

namespace cmlb::test_support {

class InMemoryUserSettingsRepository final
    : public cmlb::infrastructure::persistence::UserSettingsRepository {
public:
    InMemoryUserSettingsRepository()  = default;
    ~InMemoryUserSettingsRepository() override = default;

    boost::asio::awaitable<cmlb::core::Result<
        std::optional<cmlb::infrastructure::persistence::UserSettingsRecord>>>
    get(cmlb::domain::UserId user) override {
        std::lock_guard lk{mutex_};
        auto it = store_.find(user);
        if (it == store_.end()) {
            co_return std::optional<
                cmlb::infrastructure::persistence::UserSettingsRecord>{};
        }
        co_return std::optional<
            cmlb::infrastructure::persistence::UserSettingsRecord>{it->second};
    }

    boost::asio::awaitable<cmlb::core::Result<void>>
    save(cmlb::infrastructure::persistence::UserSettingsRecord record)
        override {
        std::lock_guard lk{mutex_};
        const auto user = record.user_id;
        store_.insert_or_assign(user, std::move(record));
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>>
    remove(cmlb::domain::UserId user) override {
        std::lock_guard lk{mutex_};
        if (store_.erase(user) == 0) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                        "user settings not found");
        }
        co_return cmlb::core::Result<void>{};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lk{mutex_};
        return store_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<cmlb::domain::UserId,
             cmlb::infrastructure::persistence::UserSettingsRecord>
        store_;
};

}  // namespace cmlb::test_support
