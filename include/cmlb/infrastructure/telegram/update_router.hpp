#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/executor.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/telegram_gateway.hpp>

/// @file update_router.hpp
/// @brief Demultiplexes `TelegramGateway::UpdateHandlers` into per-update-type
///        handlers registered by individual use cases.
///
/// The gateway speaks a flat handler bundle; use cases want to register their
/// own coroutine for new messages while a status renderer might subscribe to
/// callback queries or file progress separately. `UpdateRouter` glues those
/// concerns together. For awaitable handlers it `co_spawn`s onto the executor
/// and detaches — handlers therefore must not assume serial execution between
/// updates of the same type.

namespace cmlb::infrastructure::telegram {

class UpdateRouter {
public:
    /// Coroutine invoked on every new text message.
    using NewMessageHandler = std::function<boost::asio::awaitable<void>(
        domain::ChatId chat, domain::UserId sender, domain::MessageId msg_id, std::string text)>;

    /// Coroutine invoked on every inline-button press. `msg_id` identifies
    /// the message that owns the inline keyboard (so handlers can edit or
    /// delete it from `close`/`status:refresh`/etc payloads).
    using CallbackQueryHandler =
        std::function<boost::asio::awaitable<void>(domain::ChatId chat,
                                                   domain::UserId sender,
                                                   domain::MessageId msg_id,
                                                   domain::CallbackQueryId query_id,
                                                   std::string data)>;

    /// Synchronous handler invoked on file progress updates. These arrive at
    /// high frequency; the handler should be non-blocking (e.g. update an
    /// atomic snapshot used by a status renderer).
    using FileUpdateHandler = std::function<void(domain::FileId file_id,
                                                 std::int64_t total,
                                                 std::int64_t downloaded,
                                                 std::int64_t uploaded,
                                                 bool is_uploading_active,
                                                 bool is_downloading_active,
                                                 bool is_uploading_completed,
                                                 bool is_downloading_completed)>;

    /// Wires the router's internal dispatcher into the gateway. The gateway's
    /// previous update handlers are *replaced*. Lifetime: both `gateway` and
    /// `executor` must outlive this router.
    UpdateRouter(TelegramGateway& gateway, core::Executor& executor);

    ~UpdateRouter() = default;

    UpdateRouter(const UpdateRouter&) = delete;
    UpdateRouter& operator=(const UpdateRouter&) = delete;
    UpdateRouter(UpdateRouter&&) = delete;
    UpdateRouter& operator=(UpdateRouter&&) = delete;

    /// Registers the new-message handler. The previous one (if any) is
    /// replaced.
    void on_new_message(NewMessageHandler handler);

    /// Registers the callback-query handler.
    void on_callback_query(CallbackQueryHandler handler);

    /// Registers the file-progress handler.
    void on_file_update(FileUpdateHandler handler);

private:
    /// Pushes the current handler bundle to the gateway. Invoked from every
    /// `on_*` setter so registration order does not matter.
    void rewire_gateway();

    TelegramGateway& gateway_;
    core::Executor& executor_;

    NewMessageHandler new_message_handler_;
    CallbackQueryHandler callback_query_handler_;
    FileUpdateHandler file_update_handler_;
};

} // namespace cmlb::infrastructure::telegram
