#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/upload/drive_resource_operations.hpp>

namespace cmlb::test_support {

/// Programmable Drive resource operations stub for clone/count/delete use cases.
class StubDriveResourceOperations final
    : public cmlb::infrastructure::upload::DriveResourceOperations {
public:
    StubDriveResourceOperations() = default;
    ~StubDriveResourceOperations() override = default;

    void set_copy_result(std::string id) {
        std::lock_guard lk{mutex_};
        copy_result_ = std::move(id);
    }

    void set_count_result(cmlb::infrastructure::upload::CountResult result) {
        std::lock_guard lk{mutex_};
        count_result_ = result;
    }

    void set_error(cmlb::core::AppError error) {
        std::lock_guard lk{mutex_};
        error_ = std::move(error);
    }

    [[nodiscard]] int copy_calls() const noexcept {
        std::lock_guard lk{mutex_};
        return copy_calls_;
    }

    [[nodiscard]] int count_calls() const noexcept {
        std::lock_guard lk{mutex_};
        return count_calls_;
    }

    [[nodiscard]] int remove_calls() const noexcept {
        std::lock_guard lk{mutex_};
        return remove_calls_;
    }

    boost::asio::awaitable<cmlb::core::Result<std::string>> copy(
        std::string, std::string) override {
        std::lock_guard lk{mutex_};
        ++copy_calls_;
        if (error_) {
            co_return std::unexpected(*error_);
        }
        co_return copy_result_;
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::infrastructure::upload::CountResult>> count(
        std::string) override {
        std::lock_guard lk{mutex_};
        ++count_calls_;
        if (error_) {
            co_return std::unexpected(*error_);
        }
        co_return count_result_;
    }

    boost::asio::awaitable<cmlb::core::Result<void>> remove(std::string) override {
        std::lock_guard lk{mutex_};
        ++remove_calls_;
        if (error_) {
            co_return std::unexpected(*error_);
        }
        co_return cmlb::core::Result<void>{};
    }

private:
    mutable std::mutex mutex_;
    std::optional<cmlb::core::AppError> error_;
    std::string copy_result_{"copied-id"};
    cmlb::infrastructure::upload::CountResult count_result_{.files = 3,
                                                            .folders = 2,
                                                            .total_bytes = 4096};
    int copy_calls_{0};
    int count_calls_{0};
    int remove_calls_{0};
};

} // namespace cmlb::test_support
