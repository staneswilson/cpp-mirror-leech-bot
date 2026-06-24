// ---------------------------------------------------------------------------
// count_drive_resource.cpp — CountDriveResource use case.
// ---------------------------------------------------------------------------

#include <regex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <cmlb/application/count_drive_resource.hpp>
#include <cmlb/core/formatting.hpp>
#include <cmlb/core/logger.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace tg_ns = cmlb::infrastructure::telegram;

namespace {

[[nodiscard]] std::string extract_drive_id(const std::string& input) {
    static const std::regex file_re(R"(/file/d/([a-zA-Z0-9_-]+))");
    static const std::regex folder_re(R"(/folders/([a-zA-Z0-9_-]+))");
    static const std::regex query_re(R"([?&]id=([a-zA-Z0-9_-]+))");
    std::smatch m;
    if (std::regex_search(input, m, file_re))
        return m[1].str();
    if (std::regex_search(input, m, folder_re))
        return m[1].str();
    if (std::regex_search(input, m, query_re))
        return m[1].str();
    return input;
}

} // namespace

CountDriveResource::CountDriveResource(
    cmlb::infrastructure::upload::DriveResourceOperations& gdrive,
    tg_ns::MessengerInterface& messenger) noexcept
    : gdrive_{gdrive}, messenger_{messenger} {
}

asio::awaitable<cmlb::core::Result<cmlb::infrastructure::upload::CountResult>>
CountDriveResource::execute(CountRequest request) {
    cmlb::core::Logger::info("count_drive: user={} chat={} source={}",
                             request.user.value(),
                             request.chat.value(),
                             request.source_url);

    if (request.source_url.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "count: empty source URL");
    }
    const std::string folder_id = extract_drive_id(request.source_url);

    auto progress = co_await messenger_.send_html(
        request.chat,
        "<b><u>Counting</u></b>\n<blockquote>Drive folder scan started.</blockquote>");
    if (!progress)
        co_return std::unexpected(progress.error());

    auto counted = co_await gdrive_.count(folder_id);
    if (!counted) {
        (void)co_await messenger_.edit_html(
            request.chat,
            *progress,
            fmt::format("<b><u>Count Failed</u></b>\n<blockquote>{}</blockquote>",
                        cmlb::core::escape_html(counted.error().message)));
        co_return std::unexpected(counted.error());
    }

    (void)co_await messenger_.edit_html(
        request.chat,
        *progress,
        fmt::format("<b><u>Drive Count</u></b>\n"
                    "<b>Files:</b> <code>{}</code>\n"
                    "<b>Folders:</b> <code>{}</code>\n"
                    "<b>Total:</b> <code>{}</code>",
                    counted->files,
                    counted->folders,
                    cmlb::core::format_bytes(counted->total_bytes)));
    cmlb::core::Logger::info("count_drive: files={} folders={} bytes={}",
                             counted->files,
                             counted->folders,
                             counted->total_bytes);
    co_return *counted;
}

} // namespace cmlb::application
