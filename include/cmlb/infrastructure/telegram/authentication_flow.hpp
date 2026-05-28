#pragma once

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/telegram/telegram_gateway.hpp>

/// @file authentication_flow.hpp
/// @brief Drives the TDLib authentication state machine without blocking
///        stdin.
///
/// The legacy bot performed authentication with synchronous `std::cin` /
/// `std::getline` calls scattered through the update handler. That coupled
/// the event loop to a TTY and made unattended/headless runs impossible.
///
/// For v1 we only support **bot-token** authentication. The phone-flow
/// (registration, code prompt, 2FA password) is out of scope until a
/// follow-up ADR justifies it.

namespace cmlb::infrastructure::telegram {

/// One-shot driver that progresses the TDLib auth state to
/// `authorizationStateReady`.
class AuthenticationFlow {
public:
    /// Captures references for use during `authenticate()`. The gateway must
    /// be `run()`-ing concurrently — `authenticate()` simply issues TDLib
    /// requests that the running loop completes.
    AuthenticationFlow(TelegramGateway& gateway, core::TelegramConfig config);

    ~AuthenticationFlow() = default;

    AuthenticationFlow(const AuthenticationFlow&) = delete;
    AuthenticationFlow& operator=(const AuthenticationFlow&) = delete;
    AuthenticationFlow(AuthenticationFlow&&) = delete;
    AuthenticationFlow& operator=(AuthenticationFlow&&) = delete;

    /// Walks the auth state machine to `Ready`. Returns success once the bot
    /// is fully authorised, or an `AppError` carrying the failure reason. The
    /// concrete TDLib requests issued are PIMPL-internal; see the
    /// implementation in `authentication_flow.cpp`.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> authenticate();

private:
    TelegramGateway& gateway_;
    core::TelegramConfig config_;
};

} // namespace cmlb::infrastructure::telegram
