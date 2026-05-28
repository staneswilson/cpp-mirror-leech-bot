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

#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/presentation/callback_dispatcher.hpp>

using cmlb::presentation::CallbackDispatcher;

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
