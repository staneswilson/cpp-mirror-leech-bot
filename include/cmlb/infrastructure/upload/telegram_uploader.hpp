#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/upload/uploader_interface.hpp>

/// @file telegram_uploader.hpp
/// @brief `UploaderInterface` adapter that uploads files via TDLib through
///        `infrastructure::telegram::Messenger`.

namespace cmlb::infrastructure::telegram {
class MessengerInterface; // forward decl — no `<td/...>` reaches this header.
} // namespace cmlb::infrastructure::telegram

namespace cmlb::infrastructure::upload {

/// Telegram leech adapter.
///
/// Files up to `config.split_size` are forwarded to `MessengerInterface::send_file`
/// as a single message. Larger files are split client-side into `partNNN`
/// chunks (streamed via `std::ifstream`/`std::ofstream`; never fully loaded
/// in memory) and each part is sent as a document with a `"PartNN/TOTAL name"`
/// caption.
///
/// The uploader holds a reference to `MessengerInterface` (not the concrete
/// `Messenger`) so that unit tests can inject a stub. Production code
/// constructs the concrete `Messenger` and lets implicit upcast take effect.
class TelegramUploader final : public UploaderInterface {
public:
    /// Borrows `messenger` and `telegram_config`; both must outlive this
    /// uploader. `telegram_config.upload_parallelism` controls how many split
    /// parts are kept in flight when a file exceeds `UploadConfig.split_size`.
    explicit TelegramUploader(cmlb::infrastructure::telegram::MessengerInterface& messenger,
                              const cmlb::core::TelegramConfig& telegram_config) noexcept;

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<UploadResult>> upload_file(
        std::filesystem::path path,
        UploadConfig config,
        UploadProgressHandler on_progress) override;

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::vector<UploadResult>>>
    upload_directory(std::filesystem::path path,
                     UploadConfig config,
                     UploadProgressHandler on_progress) override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "telegram";
    }

    /// Always returns true — readiness is owned by the underlying gateway,
    /// not by this uploader. Health probes belong on the gateway.
    [[nodiscard]] bool is_ready() const noexcept override {
        return true;
    }

private:
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    const cmlb::core::TelegramConfig& telegram_config_;
};

} // namespace cmlb::infrastructure::upload
