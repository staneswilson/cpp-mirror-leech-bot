// ---------------------------------------------------------------------------
// authentication_flow.cpp
//
// Drives the TDLib auth state machine to `Ready` via bot-token authentication.
// The flow registers itself as an auth-state listener on the gateway, then
// reacts to each transition by issuing the appropriate request. Completion
// is signalled by `authorizationStateReady`. Failure paths complete with
// `ErrorCode::Unauthenticated` carrying a diagnostic.
//
// We use a `boost::asio::steady_timer` as a one-shot synchronisation primitive
// — the listener cancels the timer with `expires_at(max)` when the terminal
// state arrives. This avoids pulling in `boost::asio::experimental::channel`
// just for a one-shot.
// ---------------------------------------------------------------------------

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/telegram/authentication_flow.hpp>

namespace cmlb::infrastructure::telegram {

namespace asio = boost::asio;

AuthenticationFlow::AuthenticationFlow(TelegramGateway& gateway, core::TelegramConfig config)
    : gateway_{gateway}, config_{std::move(config)} {
}

asio::awaitable<core::Result<void>> AuthenticationFlow::authenticate() {
    if (config_.bot_token.empty()) {
        co_return core::error(
            core::ErrorCode::InvalidConfiguration,
            "authentication_flow: bot_token is empty; v1 supports bot-token auth only");
    }

    // Shared state observed by the listener installed below.
    struct State {
        asio::steady_timer done;
        bool completed{false};
        bool ready{false};
        TelegramGateway::AuthState last_state{TelegramGateway::AuthState::Other};

        explicit State(asio::any_io_executor ex) : done{ex} {
        }
    };

    auto exec = co_await asio::this_coro::executor;
    auto state = std::make_shared<State>(exec);
    state->done.expires_at(std::chrono::steady_clock::time_point::max());

    gateway_.set_auth_state_handler([state, this](TelegramGateway::AuthState s) {
        state->last_state = s;
        switch (s) {
        case TelegramGateway::AuthState::WaitTdlibParameters:
            // Fire-and-forget on the gateway's strand — the response
            // simply unblocks the next state transition.
            asio::co_spawn(
                state->done.get_executor(),
                [this]() -> asio::awaitable<void> {
                    auto r = co_await gateway_.set_tdlib_parameters();
                    if (!r.has_value()) {
                        core::Logger::error("auth: set_tdlib_parameters failed: {}",
                                            r.error().message);
                    }
                    co_return;
                },
                asio::detached);
            break;
        case TelegramGateway::AuthState::WaitPhoneOrBotToken:
            asio::co_spawn(
                state->done.get_executor(),
                [this]() -> asio::awaitable<void> {
                    auto r = co_await gateway_.check_bot_token();
                    if (!r.has_value()) {
                        core::Logger::error("auth: check_bot_token failed: {}", r.error().message);
                    }
                    co_return;
                },
                asio::detached);
            break;
        case TelegramGateway::AuthState::Ready:
            state->ready = true;
            state->completed = true;
            // Push runtime throughput options. Detached because the
            // auth gate is already unblocked; the options propagate
            // to TDLib in the background and we log any failures.
            asio::co_spawn(
                state->done.get_executor(),
                [this]() -> asio::awaitable<void> {
                    auto r = co_await gateway_.apply_runtime_options();
                    if (!r.has_value()) {
                        core::Logger::warn("auth: apply_runtime_options failed: {}",
                                           r.error().message);
                    }
                    co_return;
                },
                asio::detached);
            // Modern Asio dropped `cancel(error_code&)`; the
            // no-arg form is non-throwing for timers.
            state->done.cancel();
            break;
        case TelegramGateway::AuthState::Closed:
        case TelegramGateway::AuthState::LoggingOut:
        case TelegramGateway::AuthState::Closing:
            state->ready = false;
            state->completed = true;
            // Modern Asio dropped `cancel(error_code&)`; the
            // no-arg form is non-throwing for timers.
            state->done.cancel();
            break;
        case TelegramGateway::AuthState::Other:
        default:
            break;
        }
    });

    // Block until the listener cancels the timer.
    boost::system::error_code ec;
    co_await state->done.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    // Detach the listener so subsequent auth-state updates (e.g. logout) do
    // not call into our captured state.
    gateway_.set_auth_state_handler({});

    if (!state->completed) {
        co_return core::error(core::ErrorCode::Internal,
                              "authentication_flow: timer fired before completion");
    }
    if (!state->ready) {
        co_return core::error(core::ErrorCode::Unauthenticated,
                              "authentication_flow: terminated before reaching Ready state");
    }

    core::Logger::info("authentication_flow: bot authorised");
    co_return core::Result<void>{};
}

} // namespace cmlb::infrastructure::telegram
