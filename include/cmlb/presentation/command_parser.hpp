#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <cmlb/domain/identifiers.hpp>

/// @file command_parser.hpp
/// @brief Pure parser turning raw Telegram text into a structured command.
///
/// `CommandParser` performs **no I/O**: it does not touch the messenger, the
/// authority service, or any use case. Its sole responsibility is mapping a
/// `(text, sender, chat, msg)` tuple to an optional `CommandRequest` describing
/// the command name, its arguments, and the originating context.

namespace cmlb::presentation {

/// Structured representation of a single parsed Telegram command.
///
/// Produced exclusively by `CommandParser::parse`. All fields are by-value so
/// the request can be safely moved across coroutine resumption points.
struct CommandRequest {
    /// Command name, lower-cased, **without** the leading slash and without the
    /// `@BotUsername` suffix (e.g. `"mirror"` for `/mirror@MyBot foo`).
    std::string command;
    /// Everything that followed the command on the original line, with leading
    /// and trailing whitespace trimmed. Empty when no arguments were provided.
    std::string arguments;
    /// The original message text exactly as received. Useful for debug logs and
    /// for sub-commands that want to do their own splitting (e.g. `/rss add`).
    std::string full_text;
    /// User that sent the message.
    cmlb::domain::UserId sender;
    /// Chat the message was sent in.
    cmlb::domain::ChatId chat;
    /// The originating message ID — used to anchor reply chains.
    cmlb::domain::MessageId source_message;
};

/// Pure parser. All public surface is `static`; there is intentionally no
/// instance state.
class CommandParser {
public:
    CommandParser()                                = delete;
    CommandParser(const CommandParser&)            = delete;
    CommandParser& operator=(const CommandParser&) = delete;
    CommandParser(CommandParser&&)                 = delete;
    CommandParser& operator=(CommandParser&&)      = delete;

    /// Attempts to parse @p text as a command.
    ///
    /// Returns `std::nullopt` for non-command messages — anything that doesn't
    /// start with a `/`, the lone `/` character, or text whose body collapses
    /// to an empty command name after trimming.
    ///
    /// Behaviour:
    ///  - `"/mirror"`            -> `command="mirror"`, `arguments=""`
    ///  - `"/mirror url"`        -> `command="mirror"`, `arguments="url"`
    ///  - `"/mirror@MyBot url"`  -> `command="mirror"`, `arguments="url"`
    ///  - `"/MIRROR url"`        -> `command="mirror"`, `arguments="url"`
    ///  - `"   /mirror   url  "` -> `command="mirror"`, `arguments="url"`
    ///  - `"hello"`              -> `nullopt`
    ///  - `""`                   -> `nullopt`
    ///  - `"/"`                  -> `nullopt`
    [[nodiscard]] static std::optional<CommandRequest>
        parse(std::string_view text,
              cmlb::domain::UserId sender,
              cmlb::domain::ChatId chat,
              cmlb::domain::MessageId msg);
};

}  // namespace cmlb::presentation
