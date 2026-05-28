// ---------------------------------------------------------------------------
// update_router.cpp
//
// Demultiplexes updates from `TelegramGateway::UpdateHandlers` into
// per-update-type coroutine handlers registered by use cases.
// ---------------------------------------------------------------------------

#include <exception>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/telegram/update_router.hpp>

namespace cmlb::infrastructure::telegram {

namespace asio = boost::asio;

UpdateRouter::UpdateRouter(TelegramGateway& gateway, core::Executor& executor)
    : gateway_{gateway}, executor_{executor} {
    rewire_gateway();
}

void UpdateRouter::on_new_message(NewMessageHandler handler) {
    new_message_handler_ = std::move(handler);
    rewire_gateway();
}

void UpdateRouter::on_callback_query(CallbackQueryHandler handler) {
    callback_query_handler_ = std::move(handler);
    rewire_gateway();
}

void UpdateRouter::on_file_update(FileUpdateHandler handler) {
    file_update_handler_ = std::move(handler);
    rewire_gateway();
}

void UpdateRouter::rewire_gateway() {
    TelegramGateway::UpdateHandlers bundle;

    if (new_message_handler_) {
        bundle.on_new_message = [&exec = executor_,
                                 handler = new_message_handler_](domain::ChatId chat,
                                                                 domain::UserId sender,
                                                                 domain::MessageId msg_id,
                                                                 std::string text) {
            asio::co_spawn(
                exec.get_executor(),
                handler(chat, sender, msg_id, std::move(text)),
                [](std::exception_ptr eptr) {
                    if (eptr) {
                        try {
                            std::rethrow_exception(eptr);
                        } catch (const std::exception& e) {
                            core::Logger::error("update_router: new-message handler threw: {}",
                                                e.what());
                        } catch (...) {
                            core::Logger::error("update_router: new-message handler threw unknown");
                        }
                    }
                });
        };
    }

    if (callback_query_handler_) {
        bundle.on_callback_query = [&exec = executor_, handler = callback_query_handler_](
                                       domain::ChatId chat,
                                       domain::UserId sender,
                                       domain::MessageId msg_id,
                                       domain::CallbackQueryId query_id,
                                       std::string data) {
            asio::co_spawn(exec.get_executor(),
                           handler(chat, sender, msg_id, query_id, std::move(data)),
                           [](std::exception_ptr eptr) {
                               if (eptr) {
                                   try {
                                       std::rethrow_exception(eptr);
                                   } catch (const std::exception& e) {
                                       core::Logger::error(
                                           "update_router: callback-query handler threw: {}",
                                           e.what());
                                   } catch (...) {
                                       core::Logger::error(
                                           "update_router: callback-query handler threw unknown");
                                   }
                               }
                           });
        };
    }

    if (file_update_handler_) {
        bundle.on_file_update = file_update_handler_;
    }

    gateway_.set_update_handlers(std::move(bundle));
}

} // namespace cmlb::infrastructure::telegram
