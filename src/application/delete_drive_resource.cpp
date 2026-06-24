// ---------------------------------------------------------------------------
// delete_drive_resource.cpp — DeleteDriveResource use case.
// ---------------------------------------------------------------------------

#include <regex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <cmlb/application/delete_drive_resource.hpp>
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

DeleteDriveResource::DeleteDriveResource(cmlb::infrastructure::upload::GoogleDriveUploader& gdrive,
                                         tg_ns::MessengerInterface& messenger) noexcept
    : gdrive_{gdrive}, messenger_{messenger} {
}

asio::awaitable<cmlb::core::Result<void>> DeleteDriveResource::execute(DeleteDriveRequest request) {
    cmlb::core::Logger::info("delete_drive: user={} chat={} source={}",
                             request.user.value(),
                             request.chat.value(),
                             request.source_url);

    if (request.source_url.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "delete: empty source URL");
    }
    const std::string file_id = extract_drive_id(request.source_url);

    auto removed = co_await gdrive_.remove(file_id);
    if (!removed) {
        (void)co_await messenger_.send_html(
            request.chat,
            fmt::format("<b><u>Delete Failed</u></b>\n<blockquote>{}</blockquote>",
                        cmlb::core::escape_html(removed.error().message)));
        cmlb::core::Logger::warn("delete_drive: {}", removed.error().message);
        co_return std::unexpected(removed.error());
    }

    (void)co_await messenger_.send_html(
        request.chat,
        fmt::format("<b><u>Deleted</u></b>\n<b>Drive id:</b> <code>{}</code>",
                    cmlb::core::escape_html(file_id)));
    cmlb::core::Logger::info("delete_drive: removed id={}", file_id);
    co_return cmlb::core::Result<void>{};
}

} // namespace cmlb::application
