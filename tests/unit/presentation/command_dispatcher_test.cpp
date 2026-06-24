// ---------------------------------------------------------------------------
// command_dispatcher_test.cpp - unit tests for command fan-out UX behavior.
// ---------------------------------------------------------------------------

#include "in_memory_bot_settings_repository.hpp"
#include "in_memory_rss_feed_repository.hpp"
#include "in_memory_task_repository.hpp"
#include "in_memory_user_settings_repository.hpp"
#include "stub_downloader.hpp"
#include "stub_drive_resource_operations.hpp"
#include "stub_messenger.hpp"
#include "stub_uploader.hpp"

#include <array>
#include <chrono>
#include <span>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/cancel_task.hpp>
#include <cmlb/application/clone_drive_resource.hpp>
#include <cmlb/application/count_drive_resource.hpp>
#include <cmlb/application/delete_drive_resource.hpp>
#include <cmlb/application/leech_url.hpp>
#include <cmlb/application/mirror_url.hpp>
#include <cmlb/application/pause_task.hpp>
#include <cmlb/application/progress_renderer_interface.hpp>
#include <cmlb/application/resume_task.hpp>
#include <cmlb/application/rss_subscription.hpp>
#include <cmlb/application/show_stats.hpp>
#include <cmlb/application/show_status.hpp>
#include <cmlb/application/update_bot_settings.hpp>
#include <cmlb/application/update_user_settings.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/domain/authority.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/presentation/command_dispatcher.hpp>

namespace asio = boost::asio;

using cmlb::application::ActiveTaskRegistry;
using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::UserId;
using cmlb::presentation::CommandDispatcher;
using cmlb::presentation::CommandRequest;
using cmlb::test_support::InMemoryBotSettingsRepository;
using cmlb::test_support::InMemoryRssFeedRepository;
using cmlb::test_support::InMemoryTaskRepository;
using cmlb::test_support::InMemoryUserSettingsRepository;
using cmlb::test_support::StubDownloader;
using cmlb::test_support::StubDriveResourceOperations;
using cmlb::test_support::StubMessenger;
using cmlb::test_support::StubUploader;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr UserId OWNER{1};
constexpr UserId USER{42};
constexpr ChatId CHAT{-100123};
constexpr MessageId SOURCE_MESSAGE{777};

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

class NullProgressRenderer final : public cmlb::application::ProgressRendererInterface {
public:
    boost::asio::awaitable<cmlb::core::Result<void>> render(
        cmlb::domain::ChatId,
        std::span<const cmlb::infrastructure::download::DownloadStatus>) override {
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>> force_refresh(
        cmlb::domain::ChatId,
        std::span<const cmlb::infrastructure::download::DownloadStatus>) override {
        co_return cmlb::core::Result<void>{};
    }
};

struct DispatcherFixture {
    cmlb::core::Executor executor{std::size_t{1}};
    StubDownloader aria2;
    StubDownloader qbit;
    StubUploader gdrive_uploader{"gdrive"};
    StubUploader rclone_uploader{"rclone"};
    StubUploader telegram_uploader{"telegram"};
    StubDriveResourceOperations drive_resources;
    InMemoryTaskRepository tasks;
    InMemoryUserSettingsRepository user_settings_repo;
    InMemoryBotSettingsRepository bot_settings_repo;
    InMemoryRssFeedRepository rss_repo;
    StubMessenger messenger;
    cmlb::infrastructure::system::SystemMetrics metrics;
    NullProgressRenderer progress_renderer;
    ActiveTaskRegistry active_tasks;

    cmlb::application::MirrorUrl mirror_url{aria2,
                                            qbit,
                                            gdrive_uploader,
                                            rclone_uploader,
                                            tasks,
                                            user_settings_repo,
                                            messenger,
                                            progress_renderer,
                                            executor,
                                            active_tasks};
    cmlb::application::LeechUrl leech_url{aria2,
                                          qbit,
                                          telegram_uploader,
                                          tasks,
                                          user_settings_repo,
                                          messenger,
                                          progress_renderer,
                                          executor,
                                          active_tasks};
    cmlb::application::CloneDriveResource clone{drive_resources, messenger, "target-folder"};
    cmlb::application::CountDriveResource count{drive_resources, messenger};
    cmlb::application::DeleteDriveResource delete_resource{drive_resources, messenger};
    cmlb::application::CancelTask cancel_task{tasks, aria2, qbit, messenger, active_tasks};
    cmlb::application::PauseTask pause_task{tasks, aria2, qbit, messenger};
    cmlb::application::ResumeTask resume_task{tasks, aria2, qbit, messenger};
    cmlb::application::ShowStats show_stats{
        aria2, qbit, metrics, std::chrono::steady_clock::now()};
    cmlb::application::ShowStatus show_status{
        tasks, aria2, qbit, messenger, metrics, std::chrono::steady_clock::now()};
    cmlb::application::UpdateUserSettings update_user{user_settings_repo};
    cmlb::application::UpdateBotSettings update_bot{bot_settings_repo};
    cmlb::application::RssSubscription rss{rss_repo};

    [[nodiscard]] CommandDispatcher make_dispatcher() {
        const std::array<UserId, 0> sudo{};
        const std::array<ChatId, 1> chats{CHAT};
        return CommandDispatcher{CommandDispatcher::Dependencies{
            .authority =
                cmlb::domain::Authority{OWNER, std::span<const UserId>{sudo}, chats},
            .mirror_url = mirror_url,
            .leech_url = leech_url,
            .clone = clone,
            .count = count,
            .delete_resource = delete_resource,
            .cancel_task = cancel_task,
            .pause_task = pause_task,
            .resume_task = resume_task,
            .show_stats = show_stats,
            .show_status = show_status,
            .update_user = update_user,
            .update_bot = update_bot,
            .rss = rss,
            .messenger = messenger,
        }};
    }
};

[[nodiscard]] CommandRequest request(std::string command,
                                     std::string arguments,
                                     UserId sender = USER) {
    return CommandRequest{
        .command = std::move(command),
        .arguments = std::move(arguments),
        .full_text = {},
        .sender = sender,
        .chat = CHAT,
        .source_message = SOURCE_MESSAGE,
    };
}

} // namespace

TEST_CASE("Drive commands do not send duplicate dispatcher completion messages",
          "[presentation][command_dispatcher]") {
    asio::io_context ctx;

    SECTION("clone edits the use-case progress message to completion") {
        DispatcherFixture fixture;
        auto dispatcher = fixture.make_dispatcher();

        const auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await dispatcher.dispatch(request("clone", "source-folder"));
        });

        REQUIRE(result.has_value());
        CHECK(fixture.drive_resources.copy_calls() == 1);
        CHECK(fixture.messenger.sends().size() == 1);
        CHECK(fixture.messenger.edits().size() == 1);
    }

    SECTION("count edits the use-case progress message to completion") {
        DispatcherFixture fixture;
        auto dispatcher = fixture.make_dispatcher();

        const auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await dispatcher.dispatch(request("count", "source-folder"));
        });

        REQUIRE(result.has_value());
        CHECK(fixture.drive_resources.count_calls() == 1);
        CHECK(fixture.messenger.sends().size() == 1);
        const auto edits = fixture.messenger.edits();
        REQUIRE(edits.size() == 1);
        CHECK_THAT(edits.front().html, ContainsSubstring("<b>Total:</b> <code>4.00 KiB</code>"));
        CHECK_THAT(edits.front().html, !ContainsSubstring("<b>Bytes:</b>"));
    }

    SECTION("del leaves the single use-case result message") {
        DispatcherFixture fixture;
        auto dispatcher = fixture.make_dispatcher();

        const auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await dispatcher.dispatch(request("del", "source-file", OWNER));
        });

        REQUIRE(result.has_value());
        CHECK(fixture.drive_resources.remove_calls() == 1);
        CHECK(fixture.messenger.sends().size() == 1);
        CHECK(fixture.messenger.edits().empty());
    }
}
