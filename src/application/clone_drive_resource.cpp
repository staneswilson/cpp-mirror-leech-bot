// ---------------------------------------------------------------------------
// clone_drive_resource.cpp — CloneDriveResource use case.
// ---------------------------------------------------------------------------

#include <cmlb/application/clone_drive_resource.hpp>

#include <regex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <cmlb/core/logger.hpp>

namespace cmlb::application {

namespace asio  = boost::asio;
namespace tg_ns = cmlb::infrastructure::telegram;

namespace {

/// Extracts the Drive id from a public URL, or returns @p input unchanged
/// when it already looks like a bare id.
[[nodiscard]] std::string extract_drive_id(const std::string& input) {
    static const std::regex file_re(R"(/file/d/([a-zA-Z0-9_-]+))");
    static const std::regex folder_re(R"(/folders/([a-zA-Z0-9_-]+))");
    static const std::regex query_re(R"([?&]id=([a-zA-Z0-9_-]+))");
    std::smatch m;
    if (std::regex_search(input, m, file_re))   return m[1].str();
    if (std::regex_search(input, m, folder_re)) return m[1].str();
    if (std::regex_search(input, m, query_re))  return m[1].str();
    return input;
}

}  // namespace

CloneDriveResource::CloneDriveResource(
    cmlb::infrastructure::upload::GoogleDriveUploader& gdrive,
    tg_ns::MessengerInterface& messenger,
    std::string target_folder_id) noexcept
    : gdrive_{gdrive},
      messenger_{messenger},
      target_folder_id_{std::move(target_folder_id)} {}

asio::awaitable<cmlb::core::Result<std::string>>
CloneDriveResource::execute(CloneRequest request) {
    cmlb::core::Logger::info("clone_drive: user={} chat={} source={}",
                             request.user.value(), request.chat.value(),
                             request.source_url);

    if (request.source_url.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "clone: empty source URL");
    }
    if (target_folder_id_.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidConfiguration,
                                    "clone: target folder id not configured");
    }

    const std::string source_id = extract_drive_id(request.source_url);

    auto progress = co_await messenger_.send_html(
        request.chat, "<b>Cloning</b> on Drive…");
    if (!progress) co_return std::unexpected(progress.error());

    auto copied = co_await gdrive_.copy(source_id, target_folder_id_);
    if (!copied) {
        (void)co_await messenger_.edit_html(
            request.chat, *progress,
            fmt::format("<b>Clone failed</b>: {}", copied.error().message));
        cmlb::core::Logger::error("clone_drive: {}", copied.error().message);
        co_return std::unexpected(copied.error());
    }

    (void)co_await messenger_.edit_html(
        request.chat, *progress,
        fmt::format("<b>Cloned</b>: <a href=\"https://drive.google.com/"
                    "open?id={0}\">{0}</a>",
                    *copied));
    cmlb::core::Logger::info("clone_drive: new id={}", *copied);
    co_return *copied;
}

}  // namespace cmlb::application
