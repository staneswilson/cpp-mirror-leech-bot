// ---------------------------------------------------------------------------
// callback_dispatcher_test.cpp
//
// Smoke tests for `cmlb::presentation::CallbackDispatcher`.
//
// The dispatcher requires references to live application use cases and to a
// `Messenger`. Spinning up real instances of those collaborators is outside
// the scope of a pure unit test, so this file is deliberately minimal -
// confirming the basic compile-time wiring of the public API while the more
// complete behavioural coverage lives in higher-level integration tests.
//
// Once Agent M's application use cases ship (`cmlb::application::CancelTask`
// et al.), the assertions below can be extended with mock collaborators that
// observe routing decisions. Until then this file's value is build-time:
// detecting any drift in `CallbackDispatcher`'s public surface.
// ---------------------------------------------------------------------------

#include "in_memory_task_repository.hpp"
#include "in_memory_user_settings_repository.hpp"
#include "stub_downloader.hpp"
#include "stub_messenger.hpp"

#include <type_traits>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/cancel_task.hpp>
#include <cmlb/application/pause_task.hpp>
#include <cmlb/application/resume_task.hpp>
#include <cmlb/application/update_user_settings.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/presentation/callback_dispatcher.hpp>

namespace asio = boost::asio;

using Catch::Matchers::ContainsSubstring;
using cmlb::application::ActiveTaskRegistry;
using cmlb::domain::CallbackQueryId;
using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::UserId;
using cmlb::presentation::CallbackDispatcher;
using cmlb::test_support::InMemoryTaskRepository;
using cmlb::test_support::InMemoryUserSettingsRepository;
using cmlb::test_support::StubDownloader;
using cmlb::test_support::StubMessenger;

namespace {

constexpr ChatId CHAT{-100123};
constexpr UserId USER{42};
constexpr MessageId SETTINGS_MESSAGE{1001};
constexpr CallbackQueryId QUERY{9001};

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

struct CallbackFixture {
    InMemoryTaskRepository tasks;
    StubDownloader aria2;
    StubDownloader qbit;
    InMemoryUserSettingsRepository user_settings;
    StubMessenger messenger;
    ActiveTaskRegistry active_tasks;

    cmlb::application::CancelTask cancel_task{tasks, aria2, qbit, messenger, active_tasks};
    cmlb::application::PauseTask pause_task{tasks, aria2, qbit, messenger};
    cmlb::application::ResumeTask resume_task{tasks, aria2, qbit, messenger};
    cmlb::application::UpdateUserSettings update_user{user_settings};

    [[nodiscard]] CallbackDispatcher make_dispatcher() {
        return CallbackDispatcher{CallbackDispatcher::Dependencies{
            .cancel_task = cancel_task,
            .pause_task = pause_task,
            .resume_task = resume_task,
            .update_user = update_user,
            .messenger = messenger,
        }};
    }
};

} // namespace

TEST_CASE("CallbackDispatcher::Dependencies is non-copyable", "[presentation][callback]") {
    // Static compile-time checks against the dispatcher's intended ownership
    // semantics. The dispatcher is move-/copy-disabled by design; if a future
    // change introduces a move constructor, callers depending on stable
    // address (e.g. captured references) would silently break.
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<CallbackDispatcher>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<CallbackDispatcher>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<CallbackDispatcher>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<CallbackDispatcher>);
}

TEST_CASE("CallbackDispatcher::Dependencies is a plain aggregate", "[presentation][callback]") {
    // The struct must remain an aggregate of references so callers can
    // construct it with brace-init without inventing constructors.
    STATIC_REQUIRE(std::is_aggregate_v<CallbackDispatcher::Dependencies>);
}

TEST_CASE("settings callbacks edit the existing settings panel", "[presentation][callback]") {
    asio::io_context ctx;

    SECTION("upload destination cycle refreshes the panel text") {
        CallbackFixture fixture;
        auto dispatcher = fixture.make_dispatcher();

        const auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await dispatcher.dispatch(
                CHAT, USER, SETTINGS_MESSAGE, QUERY, "settings:upload:cycle");
        });

        REQUIRE(result.has_value());
        CHECK(fixture.messenger.sends().empty());
        CHECK(fixture.messenger.edits().empty());
        const auto edits = fixture.messenger.keyboard_edits();
        REQUIRE(edits.size() == 1);
        CHECK(edits.front().msg == SETTINGS_MESSAGE);
        CHECK(edits.front().keyboard.size() == 2);
        CHECK_THAT(edits.front().html,
                   ContainsSubstring("<b>Mirror destination:</b> <code>rclone</code>"));
    }

    SECTION("document mode toggle refreshes the panel text") {
        CallbackFixture fixture;
        auto dispatcher = fixture.make_dispatcher();

        const auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await dispatcher.dispatch(
                CHAT, USER, SETTINGS_MESSAGE, QUERY, "settings:doc:toggle");
        });

        REQUIRE(result.has_value());
        CHECK(fixture.messenger.sends().empty());
        CHECK(fixture.messenger.edits().empty());
        const auto edits = fixture.messenger.keyboard_edits();
        REQUIRE(edits.size() == 1);
        CHECK(edits.front().msg == SETTINGS_MESSAGE);
        CHECK(edits.front().keyboard.size() == 2);
        CHECK_THAT(edits.front().html,
                   ContainsSubstring("<b>Upload as document:</b> <code>yes</code>"));
    }
}
