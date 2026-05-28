// ---------------------------------------------------------------------------
// src/infrastructure/system/system_metrics.cpp
//
// Per-platform implementation of the SystemMetrics sampler.
//
// Implementation strategy:
//   * Linux  : parse `/proc/stat`, `/proc/meminfo`, `/proc/uptime`,
//              `getloadavg`, `statvfs`.
//   * macOS  : `host_statistics64` + `sysctlbyname` for memory/uptime,
//              `getloadavg` + `statvfs`.
//   * Windows: `GetSystemTimes`, `GlobalMemoryStatusEx`,
//              `GetTickCount64`, `GetDiskFreeSpaceExW`. No load averages.
//
// All readers degrade gracefully: an unreadable counter logs a warning
// and contributes zero rather than aborting the snapshot.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>

#if defined(__linux__)
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(_WIN32)
// clang-format off
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
// MSYS2's libstdc++ (and some Windows SDK headers) predefine NOMINMAX. Guard
// our definition so -Wpedantic / -Werror doesn't fail on a benign redefine.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <sysinfoapi.h>
// clang-format on
#endif

namespace cmlb::infrastructure::system {

namespace {

#if defined(__linux__)

bool read_cpu_ticks(std::uint64_t& idle_out, std::uint64_t& total_out) {
    std::ifstream stat{"/proc/stat"};
    if (!stat) {
        cmlb::core::Logger::warn("SystemMetrics: failed to open /proc/stat; reporting zero CPU%");
        return false;
    }
    std::string label;
    stat >> label;
    if (label != "cpu") {
        return false;
    }
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0;
    std::uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
    stat >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    idle_out = idle + iowait;
    total_out = user + nice + system + idle + iowait + irq + softirq + steal;
    return true;
}

void read_memory_linux(std::int64_t& used, std::int64_t& total) {
    std::ifstream f{"/proc/meminfo"};
    if (!f) {
        cmlb::core::Logger::warn("SystemMetrics: failed to open /proc/meminfo");
        return;
    }
    std::int64_t total_kb = 0;
    std::int64_t avail_kb = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::istringstream iss{line.substr(9)};
            iss >> total_kb;
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream iss{line.substr(13)};
            iss >> avail_kb;
        }
    }
    total = total_kb * 1024;
    used = (total_kb - avail_kb) * 1024;
    if (used < 0) {
        used = 0;
    }
}

std::chrono::seconds read_system_uptime_linux() {
    std::ifstream f{"/proc/uptime"};
    if (!f) {
        return std::chrono::seconds{0};
    }
    double secs = 0.0;
    f >> secs;
    return std::chrono::seconds{static_cast<std::int64_t>(secs)};
}

#elif defined(__APPLE__)

bool read_cpu_ticks_mac(std::uint64_t& idle_out, std::uint64_t& total_out) {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(
            mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count)
        != KERN_SUCCESS) {
        return false;
    }
    idle_out = info.cpu_ticks[CPU_STATE_IDLE];
    total_out = info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM]
                + info.cpu_ticks[CPU_STATE_NICE] + info.cpu_ticks[CPU_STATE_IDLE];
    return true;
}

void read_memory_mac(std::int64_t& used, std::int64_t& total) {
    std::int64_t mem_size = 0;
    std::size_t len = sizeof(mem_size);
    if (sysctlbyname("hw.memsize", &mem_size, &len, nullptr, 0) == 0) {
        total = mem_size;
    }
    vm_size_t page_size = 0;
    host_page_size(mach_host_self(), &page_size);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(
            mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count)
        == KERN_SUCCESS) {
        const std::int64_t free_pages =
            static_cast<std::int64_t>(vm.free_count) + vm.inactive_count;
        used = total - free_pages * static_cast<std::int64_t>(page_size);
        if (used < 0) {
            used = 0;
        }
    }
}

std::chrono::seconds read_system_uptime_mac() {
    struct timeval boot {};

    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    std::size_t size = sizeof(boot);
    if (sysctl(mib, 2, &boot, &size, nullptr, 0) != 0) {
        return std::chrono::seconds{0};
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now);
    return std::chrono::seconds{now_s.count() - boot.tv_sec};
}

#elif defined(_WIN32)

std::uint64_t filetime_to_uint64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

bool read_cpu_ticks_win(std::uint64_t& idle_out, std::uint64_t& total_out) {
    FILETIME idle_ft{}, kernel_ft{}, user_ft{};
    if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        cmlb::core::Logger::warn("SystemMetrics: GetSystemTimes failed; reporting zero CPU%");
        return false;
    }
    const auto idle = filetime_to_uint64(idle_ft);
    const auto kernel = filetime_to_uint64(kernel_ft);
    const auto user = filetime_to_uint64(user_ft);
    idle_out = idle;
    total_out = kernel + user; // kernel includes idle on Windows
    return true;
}

void read_memory_win(std::int64_t& used, std::int64_t& total) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        cmlb::core::Logger::warn("SystemMetrics: GlobalMemoryStatusEx failed");
        return;
    }
    total = static_cast<std::int64_t>(ms.ullTotalPhys);
    used = static_cast<std::int64_t>(ms.ullTotalPhys - ms.ullAvailPhys);
}

std::chrono::seconds read_system_uptime_win() {
    const std::uint64_t ticks_ms = GetTickCount64();
    return std::chrono::seconds{static_cast<std::int64_t>(ticks_ms / 1000)};
}

#endif

void read_disk(std::int64_t& used, std::int64_t& total) {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        return;
    }
    auto info = std::filesystem::space(cwd, ec);
    if (ec) {
        cmlb::core::Logger::warn(
            "SystemMetrics: filesystem::space failed for '{}': {}", cwd.string(), ec.message());
        return;
    }
    total = static_cast<std::int64_t>(info.capacity);
    used = static_cast<std::int64_t>(info.capacity - info.available);
    if (used < 0) {
        used = 0;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SystemMetrics
// ---------------------------------------------------------------------------

SystemMetrics::SystemMetrics() : start_time_{std::chrono::steady_clock::now()} {
    // Prime the CPU baseline so the next snapshot returns a real delta.
    std::lock_guard lock{cpu_mutex_};
#if defined(__linux__)
    has_prev_sample_ = read_cpu_ticks(prev_idle_ticks_, prev_total_ticks_);
#elif defined(__APPLE__)
    has_prev_sample_ = read_cpu_ticks_mac(prev_idle_ticks_, prev_total_ticks_);
#elif defined(_WIN32)
    has_prev_sample_ = read_cpu_ticks_win(prev_idle_ticks_, prev_total_ticks_);
#else
    has_prev_sample_ = false;
#endif
}

SystemSnapshot SystemMetrics::snapshot() const {
    SystemSnapshot snap;
    snap.bot_uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_);

#if defined(__linux__) || defined(__APPLE__)
    double loads[3] = {0.0, 0.0, 0.0};
    const int n = ::getloadavg(loads, 3);
    if (n >= 1)
        snap.load_average_1m = loads[0];
    if (n >= 2)
        snap.load_average_5m = loads[1];
    if (n >= 3)
        snap.load_average_15m = loads[2];
#endif

#if defined(__linux__)
    {
        std::lock_guard lock{cpu_mutex_};
        std::uint64_t idle = 0, total = 0;
        if (read_cpu_ticks(idle, total) && has_prev_sample_) {
            const auto d_idle = idle - prev_idle_ticks_;
            const auto d_total = total - prev_total_ticks_;
            if (d_total > 0) {
                snap.cpu_usage_percent =
                    100.0 * static_cast<double>(d_total - d_idle) / static_cast<double>(d_total);
            }
            prev_idle_ticks_ = idle;
            prev_total_ticks_ = total;
            has_prev_sample_ = true;
        }
    }
    read_memory_linux(snap.ram_used_bytes, snap.ram_total_bytes);
    snap.system_uptime = read_system_uptime_linux();
#elif defined(__APPLE__)
    {
        std::lock_guard lock{cpu_mutex_};
        std::uint64_t idle = 0, total = 0;
        if (read_cpu_ticks_mac(idle, total) && has_prev_sample_) {
            const auto d_idle = idle - prev_idle_ticks_;
            const auto d_total = total - prev_total_ticks_;
            if (d_total > 0) {
                snap.cpu_usage_percent =
                    100.0 * static_cast<double>(d_total - d_idle) / static_cast<double>(d_total);
            }
            prev_idle_ticks_ = idle;
            prev_total_ticks_ = total;
            has_prev_sample_ = true;
        }
    }
    read_memory_mac(snap.ram_used_bytes, snap.ram_total_bytes);
    snap.system_uptime = read_system_uptime_mac();
#elif defined(_WIN32)
    {
        std::lock_guard lock{cpu_mutex_};
        std::uint64_t idle = 0, total = 0;
        if (read_cpu_ticks_win(idle, total) && has_prev_sample_) {
            const auto d_idle = idle - prev_idle_ticks_;
            const auto d_total = total - prev_total_ticks_;
            if (d_total > 0) {
                snap.cpu_usage_percent =
                    100.0 * static_cast<double>(d_total - d_idle) / static_cast<double>(d_total);
            }
            prev_idle_ticks_ = idle;
            prev_total_ticks_ = total;
            has_prev_sample_ = true;
        }
    }
    read_memory_win(snap.ram_used_bytes, snap.ram_total_bytes);
    snap.system_uptime = read_system_uptime_win();
#endif

    read_disk(snap.disk_used_bytes, snap.disk_total_bytes);

    return snap;
}

} // namespace cmlb::infrastructure::system
