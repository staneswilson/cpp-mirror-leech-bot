#ifndef CMLB_CORE_TYPES_HPP
#define CMLB_CORE_TYPES_HPP

#include <expected>
#include <string>
#include <string_view>
#include <variant>

namespace cmlb {

/**
 * @brief application-specific error types.
 */
enum class ErrorCode {
    None = 0,
    NetworkError,
    JsonParseError,
    Aria2ConnectionFailed,
    TelegramApiError,
    InternalError
};

/**
 * @brief Structured error information.
 */
struct AppError {
    ErrorCode code;
    std::string message;

    AppError(ErrorCode c, std::string_view msg) : code(c), message(msg) {}
};

/**
 * @brief Result type alias using std::expected (C++23).
 * 
 * We use std::expected for robust error handling without exceptions for control flow.
 * It forces the caller to handle the error or propagate it.
 */
template <typename T>
using Result = std::expected<T, AppError>;

} // namespace cmlb

#endif // CMLB_CORE_TYPES_HPP
