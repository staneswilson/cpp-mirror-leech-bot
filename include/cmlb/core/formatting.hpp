#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <cmlb/core/error.hpp>

namespace cmlb::core {

/// Binary (IEC) byte unit: e.g. `format_bytes(1536) -> "1.50 KiB"`. Two
/// decimal places. Negative values are formatted with a leading minus.
[[nodiscard]] std::string format_bytes(std::int64_t bytes);

/// SI byte unit: e.g. `format_decimal_bytes(1500) -> "1.50 KB"`. Two decimals.
[[nodiscard]] std::string format_decimal_bytes(std::int64_t bytes);

/// Human-readable duration, e.g. `"2h 3m 45s"`, `"45s"`, or `"0s"` for zero.
[[nodiscard]] std::string format_duration(std::chrono::seconds duration);

/// Like `format_duration` but prefixed with `~` for "approximate". Returns
/// `"--"` when the input is zero, negative, or otherwise unknowable.
[[nodiscard]] std::string format_eta(std::chrono::seconds duration);

/// `"[####------]"` style progress bar. `fraction` is clamped to `[0, 1]`;
/// `width == 0` returns `"[]"`.
[[nodiscard]] std::string render_progress_bar(double fraction,
                                              std::size_t width = 12,
                                              char filled = '#',
                                              char empty = '-');

/// Throughput: e.g. `"1.23 MiB/s"`. Negative values are treated as zero.
[[nodiscard]] std::string format_rate(std::int64_t bytes_per_second);

/// `"42.3%"` — `fraction` is clamped to `[0, 1]` before formatting.
[[nodiscard]] std::string format_percent(double fraction, int decimals = 1);

/// Escapes the three Telegram HTML-significant characters (`&`, `<`, `>`)
/// for safe inclusion in `<b>`, `<i>`, `<code>`, and `<pre>` blocks. Pure
/// string transform, lives in core so any layer producing chat copy can
/// use it without depending on the presentation layer.
[[nodiscard]] std::string escape_html(std::string_view text);

/// UTF-8-safe display clamp. If @p text fits in @p max_bytes it is returned
/// unchanged; otherwise it is truncated at the nearest UTF-8 codepoint
/// boundary (no split sequences) and `...` is appended. Used to bound the
/// width of user-controlled values rendered into chat messages.
[[nodiscard]] std::string truncate_for_display(std::string_view text, std::size_t max_bytes);

/// Returns a short, human-readable label for @p code, suitable for direct
/// inclusion in user-facing copy. Never returns a raw enumerator name —
/// for the enumerator name, use `error_code_name` from `core/error.hpp`.
[[nodiscard]] std::string_view friendly_error_label(ErrorCode code) noexcept;

} // namespace cmlb::core
