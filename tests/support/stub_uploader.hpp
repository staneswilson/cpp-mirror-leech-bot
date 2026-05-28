#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/upload/uploader_interface.hpp>

namespace cmlb::test_support {

/// Programmable UploaderInterface stub. Records every call and yields the
/// pre-staged result (or error) to its caller.
class StubUploader final : public cmlb::infrastructure::upload::UploaderInterface {
public:
    struct FileCall {
        std::filesystem::path path;
        cmlb::infrastructure::upload::UploadConfig config;
    };

    explicit StubUploader(std::string_view label = "stub") : label_{label} {
    }

    ~StubUploader() override = default;

    void set_next_result(cmlb::infrastructure::upload::UploadResult result) {
        std::lock_guard lk{mutex_};
        next_result_ = std::move(result);
    }

    void set_error(cmlb::core::AppError err) {
        std::lock_guard lk{mutex_};
        error_ = std::move(err);
    }

    [[nodiscard]] std::vector<FileCall> file_calls() const {
        std::lock_guard lk{mutex_};
        return file_calls_;
    }

    [[nodiscard]] std::vector<FileCall> directory_calls() const {
        std::lock_guard lk{mutex_};
        return directory_calls_;
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::infrastructure::upload::UploadResult>>
    upload_file(std::filesystem::path path,
                cmlb::infrastructure::upload::UploadConfig config,
                cmlb::infrastructure::upload::UploadProgressHandler) override {
        std::lock_guard lk{mutex_};
        file_calls_.push_back(FileCall{std::move(path), std::move(config)});
        if (error_) {
            cmlb::core::AppError err = *error_;
            co_return std::unexpected(err);
        }
        co_return next_result_;
    }

    boost::asio::awaitable<
        cmlb::core::Result<std::vector<cmlb::infrastructure::upload::UploadResult>>>
    upload_directory(std::filesystem::path path,
                     cmlb::infrastructure::upload::UploadConfig config,
                     cmlb::infrastructure::upload::UploadProgressHandler) override {
        std::lock_guard lk{mutex_};
        directory_calls_.push_back(FileCall{std::move(path), std::move(config)});
        if (error_) {
            cmlb::core::AppError err = *error_;
            co_return std::unexpected(err);
        }
        std::vector<cmlb::infrastructure::upload::UploadResult> out;
        out.push_back(next_result_);
        co_return out;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return label_;
    }

    [[nodiscard]] bool is_ready() const noexcept override {
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::string label_;
    cmlb::infrastructure::upload::UploadResult next_result_{.file_id = "stub-id",
                                                            .link = "stub://link",
                                                            .size = 0,
                                                            .duration =
                                                                std::chrono::milliseconds{0}};
    std::optional<cmlb::core::AppError> error_;
    std::vector<FileCall> file_calls_;
    std::vector<FileCall> directory_calls_;
};

} // namespace cmlb::test_support
