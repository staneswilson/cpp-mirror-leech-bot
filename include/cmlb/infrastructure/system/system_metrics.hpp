#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace cmlb::infrastructure::system {

/// Point-in-time snapshot of the host's resource utilization.
///
/// Populated by `SystemMetrics::snapshot()`. All byte counts are absolute;
/// `cpu_usage_percent` is averaged over the interval since the previous
/// call so the first invocation typically reports 0.
struct SystemSnapshot {
    /// CPU usage in percent (0.0 to 100.0 * num_cores), averaged since the
    /// previous `snapshot()`. First call returns 0.
    double cpu_usage_percent = 0.0;
    /// Resident RAM currently in use, in bytes.
    std::int64_t ram_used_bytes = 0;
    /// Total physical RAM, in bytes.
    std::int64_t ram_total_bytes = 0;
    /// Bytes used on the partition containing the current working directory.
    std::int64_t disk_used_bytes = 0;
    /// Total bytes on that partition.
    std::int64_t disk_total_bytes = 0;
    /// Time elapsed since the `SystemMetrics` instance was constructed.
    std::chrono::seconds bot_uptime{0};
    /// Time the host has been up. Best-effort; 0 if not available.
    std::chrono::seconds system_uptime{0};
    /// 1-minute load average. 0 on platforms that don't expose it (Windows).
    double load_average_1m = 0.0;
    /// 5-minute load average. 0 on Windows.
    double load_average_5m = 0.0;
    /// 15-minute load average. 0 on Windows.
    double load_average_15m = 0.0;
};

/// Cross-platform system-resource sampler.
///
/// One instance should be kept alive for the lifetime of the process so the
/// CPU% delta calculation has a previous sample to compare against.
///
/// Thread-safe: `snapshot()` may be called from any thread.
class SystemMetrics {
public:
    /// Records the construction time (used for `bot_uptime`) and primes the
    /// CPU-tick counters with an initial reading.
    SystemMetrics();

    /// Returns a fresh snapshot. The first call returns
    /// `cpu_usage_percent = 0` since there is no previous sample to delta
    /// against. Never throws and never fails — unreadable counters fall
    /// back to zero with a warning logged via `cmlb::core::Logger`.
    [[nodiscard]] SystemSnapshot snapshot() const;

private:
    std::chrono::steady_clock::time_point start_time_;

    // Per-platform CPU-tick state. Defined in the .cpp; only the size is
    // exposed here so the header stays platform-agnostic.
    struct CpuState;
    mutable std::mutex cpu_mutex_;
    mutable std::uint64_t prev_idle_ticks_  = 0;
    mutable std::uint64_t prev_total_ticks_ = 0;
    mutable bool          has_prev_sample_  = false;
};

}  // namespace cmlb::infrastructure::system
