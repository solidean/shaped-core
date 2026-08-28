#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/platform/system_metrics.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <unistd.h>

#endif

using namespace cc::primitive_defines;

namespace cc
{
namespace
{
cc::query_error unsupported_here()
{
    return {.status = cc::query_status::unsupported, .detail = cc::string("not available on this platform")};
}

cc::query_error read_failed(cc::string_view what)
{
    return {.status = cc::query_status::failed, .detail = cc::format("could not read {}", what)};
}

/// A busy fraction from two readings of the same counter, clamped into [0, 1].
///
/// The clamp is not cosmetic.
/// Counters are sampled per core at slightly different instants, a core can be parked between readings, and a
/// virtualized clock can run backwards.
/// Each of those produces a ratio outside the range, which a caller would draw as a bar running off the end.
f32 busy_ratio(cc::cpu_counters const& before, cc::cpu_counters const& after)
{
    auto const busy = after.busy_secs() - before.busy_secs();
    auto const total = after.total_secs() - before.total_secs();
    if (total <= 0 || busy < 0)
        return 0;
    auto const ratio = f32(busy / total);
    return ratio > 1 ? 1.0f : ratio;
}
} // namespace
} // namespace cc

// =========================================================================================================
// Windows
// =========================================================================================================
#if defined(CC_OS_WINDOWS)

namespace cc
{
namespace
{
f64 filetime_secs(FILETIME const& t)
{
    auto const ticks = (u64(t.dwHighDateTime) << 32) | u64(t.dwLowDateTime);
    return f64(ticks) * 1e-7; // FILETIME counts 100 ns intervals
}

/// One entry of NtQuerySystemInformation's SystemProcessorPerformanceInformation, which is the only route to per-core
/// times on Windows — GetSystemTimes reports the machine as a whole and nothing else.
struct processor_performance
{
    LARGE_INTEGER idle_time;
    LARGE_INTEGER kernel_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER dpc_time;
    LARGE_INTEGER interrupt_time;
    ULONG interrupt_count;
};

using query_system_information_fn = LONG(__stdcall*)(ULONG, void*, ULONG, ULONG*);

query_system_information_fn resolve_query_system_information()
{
    static auto const fn = []() -> query_system_information_fn
    {
        auto* const ntdll = ::GetModuleHandleA("ntdll.dll");
        if (ntdll == nullptr)
            return nullptr;
        return reinterpret_cast<query_system_information_fn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtQuerySystemInformation")));
    }();
    return fn;
}

/// Windows reports kernel time INCLUDING idle time, so system time is the difference.
cc::cpu_counters counters_from(f64 idle, f64 kernel, f64 user)
{
    auto const system = kernel > idle ? kernel - idle : 0.0;
    return {.user_secs = user, .system_secs = system, .idle_secs = idle};
}

cc::result<cc::cpu_counter_set, cc::query_error> read_counters()
{
    FILETIME idle = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!::GetSystemTimes(&idle, &kernel, &user))
        return cc::error(read_failed("GetSystemTimes"));

    auto out = cc::cpu_counter_set();
    out.total = counters_from(filetime_secs(idle), filetime_secs(kernel), filetime_secs(user));

    auto* const query = resolve_query_system_information();
    if (query == nullptr)
        return out; // the total is still a real answer

    auto const cores = cc::get_system_info().logical_cores();
    if (cores <= 0)
        return out;

    auto entries = cc::vector<processor_performance>();
    entries.resize_to_uninitialized(cores);

    constexpr ULONG k_system_processor_performance_information = 8;
    ULONG written = 0;
    auto const status = query(k_system_processor_performance_information, entries.data(),
                              ULONG(cores * isize(sizeof(processor_performance))), &written);

    auto const reported = isize(written) / isize(sizeof(processor_performance));
    if (status < 0 || reported <= 0)
        return out;

    for (isize i = 0; i < reported && i < entries.size(); ++i)
        out.per_core.push_back(counters_from(f64(entries[i].idle_time.QuadPart) * 1e-7,
                                             f64(entries[i].kernel_time.QuadPart) * 1e-7,
                                             f64(entries[i].user_time.QuadPart) * 1e-7));

    return out;
}

cc::result<cc::memory_usage, cc::query_error> read_memory()
{
    auto status = MEMORYSTATUSEX{};
    status.dwLength = sizeof(status);
    if (!::GlobalMemoryStatusEx(&status))
        return cc::error(read_failed("GlobalMemoryStatusEx"));

    auto out = cc::memory_usage();
    out.total_bytes = i64(status.ullTotalPhys);
    out.available_bytes = i64(status.ullAvailPhys);
    out.used_bytes = out.total_bytes - out.available_bytes;

    // ullTotalPageFile / ullAvailPageFile are the COMMIT limit and what is left of it, despite the names.
    // Subtracting the physical figures out of them to get a swap number is arithmetic on two unrelated quantities: it
    // yields a plausible total and a "free" that goes to zero as soon as commit charge exceeds free RAM, which reads as
    // a machine swapping flat out while nothing is swapping at all.
    // So the commit charge is reported as itself, and swap stays absent here.
    out.commit_limit_bytes = i64(status.ullTotalPageFile);
    out.commit_used_bytes = i64(status.ullTotalPageFile - status.ullAvailPageFile);

    return out;
}

constexpr bool k_has_cpu_load = true;
constexpr bool k_has_memory_usage = true;
} // namespace
} // namespace cc

// =========================================================================================================
// macOS and the other Darwin targets
// =========================================================================================================
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

namespace cc
{
namespace
{
cc::cpu_counters counters_from(natural_t const* ticks, f64 per_tick)
{
    return {.user_secs = f64(ticks[CPU_STATE_USER] + ticks[CPU_STATE_NICE]) * per_tick,
            .system_secs = f64(ticks[CPU_STATE_SYSTEM]) * per_tick,
            .idle_secs = f64(ticks[CPU_STATE_IDLE]) * per_tick};
}

cc::result<cc::cpu_counter_set, cc::query_error> read_counters()
{
    // Darwin counts in scheduler ticks, which are hundredths of a second on every version that has shipped.
    constexpr f64 k_per_tick = 1.0 / 100.0;

    auto info = host_cpu_load_info_data_t{};
    auto count = mach_msg_type_number_t(HOST_CPU_LOAD_INFO_COUNT);
    if (::host_statistics(::mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count)
        != KERN_SUCCESS)
        return cc::error(read_failed("host_statistics(HOST_CPU_LOAD_INFO)"));

    auto out = cc::cpu_counter_set();
    out.total = counters_from(info.cpu_ticks, k_per_tick);

    natural_t cores = 0;
    processor_info_array_t per_core = nullptr;
    mach_msg_type_number_t per_core_count = 0;
    if (::host_processor_info(::mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cores, &per_core, &per_core_count)
        == KERN_SUCCESS)
    {
        for (natural_t i = 0; i < cores; ++i)
            out.per_core.push_back(counters_from(&per_core[i * CPU_STATE_MAX], k_per_tick));

        ::vm_deallocate(::mach_task_self(), vm_address_t(per_core), per_core_count * sizeof(integer_t));
    }

    return out;
}

cc::result<cc::memory_usage, cc::query_error> read_memory()
{
    i64 total = 0;
    auto total_size = sizeof(total);
    if (::sysctlbyname("hw.memsize", &total, &total_size, nullptr, 0) != 0)
        return cc::error(read_failed("hw.memsize"));

    auto stats = vm_statistics64_data_t{};
    auto count = mach_msg_type_number_t(HOST_VM_INFO64_COUNT);
    if (::host_statistics64(::mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count)
        != KERN_SUCCESS)
        return cc::error(read_failed("host_statistics64(HOST_VM_INFO64)"));

    auto const page = i64(::getpagesize());

    auto out = cc::memory_usage();
    out.total_bytes = total;

    // Free plus what the OS can reclaim without writing anything out, which is what "available" means everywhere else.
    out.available_bytes = (i64(stats.free_count) + i64(stats.inactive_count) + i64(stats.purgeable_count)) * page;
    if (out.available_bytes > total)
        out.available_bytes = total;
    out.used_bytes = total - out.available_bytes;

    auto swap = xsw_usage{};
    auto swap_size = sizeof(swap);
    if (::sysctlbyname("vm.swapusage", &swap, &swap_size, nullptr, 0) == 0)
    {
        out.swap_total_bytes = i64(swap.xsu_total);
        out.swap_used_bytes = i64(swap.xsu_used);
    }

    return out;
}

constexpr bool k_has_cpu_load = true;
constexpr bool k_has_memory_usage = true;
} // namespace
} // namespace cc

// =========================================================================================================
// Linux and Android
// =========================================================================================================
#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

namespace cc
{
namespace
{
/// One "cpu ..." line of /proc/stat: user, nice, system, idle, iowait, irq, softirq, steal.
///
/// iowait counts as idle rather than busy, on purpose.
/// A core waiting on a disk is available to run something else, and calling it busy is what makes a machine doing
/// nothing but I/O look pinned.
bool parse_stat_line(cc::string_view line, cc::cpu_counters& out, f64 per_tick)
{
    auto rest = cc::impl::trimmed(line);
    auto const space = rest.find(' ');
    if (space < 0)
        return false;
    rest = cc::impl::trimmed(rest.subview(space));

    f64 values[8] = {};
    for (auto i = 0; i < 8; ++i)
    {
        if (rest.empty())
            break;

        auto const next = rest.find(' ');
        auto const token = next < 0 ? rest : rest.subview_clamped(0, next);
        rest = next < 0 ? cc::string_view() : cc::impl::trimmed(rest.subview(next));

        if (auto const value = cc::from_string<i64>(token); value.has_value())
            values[i] = f64(value.value());
    }

    out.user_secs = (values[0] + values[1]) * per_tick;
    out.system_secs = (values[2] + values[5] + values[6] + values[7]) * per_tick;
    out.idle_secs = (values[3] + values[4]) * per_tick;
    return true;
}

cc::result<cc::cpu_counter_set, cc::query_error> read_counters()
{
    auto const text = cc::impl::read_text_file("/proc/stat");
    if (!text.has_value())
        return cc::error(read_failed("/proc/stat"));

    auto const clock = ::sysconf(_SC_CLK_TCK);
    auto const per_tick = clock > 0 ? 1.0 / f64(clock) : 1.0 / 100.0;

    auto out = cc::cpu_counter_set();
    auto seen_total = false;

    auto rest = cc::string_view(text.value());
    auto line = cc::string_view();
    while (cc::impl::next_line(rest, line))
    {
        if (!line.starts_with("cpu"))
            continue;

        auto counters = cc::cpu_counters();
        if (!parse_stat_line(line, counters, per_tick))
            continue;

        // "cpu " is the machine; "cpu0", "cpu1", ... are the cores.
        if (line.starts_with("cpu "))
        {
            out.total = counters;
            seen_total = true;
        }
        else
        {
            out.per_core.push_back(counters);
        }
    }

    if (!seen_total)
        return cc::error(read_failed("/proc/stat: no total line"));
    return out;
}

cc::result<cc::memory_usage, cc::query_error> read_memory()
{
    auto const text = cc::impl::read_text_file("/proc/meminfo");
    if (!text.has_value())
        return cc::error(read_failed("/proc/meminfo"));

    auto const kib = [&text](cc::string_view key) -> cc::optional<i64>
    {
        auto const field = cc::impl::field_from(text.value(), key, ':');
        if (!field.has_value())
            return {};

        auto number = cc::string_view(field.value());
        if (auto const space = number.find(' '); space >= 0)
            number = number.subview_clamped(0, space);

        auto const value = cc::from_string<i64>(number);
        if (!value.has_value())
            return {};
        return value.value() * 1024;
    };

    auto const total = kib("MemTotal");
    if (!total.has_value())
        return cc::error(read_failed("/proc/meminfo: MemTotal"));

    auto out = cc::memory_usage();
    out.total_bytes = total.value();

    // MemAvailable is the kernel's own estimate and is what a dashboard wants; the free/cached sum is the fallback for
    // kernels older than 3.14.
    if (auto const available = kib("MemAvailable"); available.has_value())
        out.available_bytes = available.value();
    else
        out.available_bytes = kib("MemFree").value_or(0) + kib("Cached").value_or(0) + kib("Buffers").value_or(0);

    if (out.available_bytes > out.total_bytes)
        out.available_bytes = out.total_bytes;
    out.used_bytes = out.total_bytes - out.available_bytes;

    if (auto const swap_total = kib("SwapTotal"); swap_total.has_value())
    {
        out.swap_total_bytes = swap_total.value();
        auto const swap_free = kib("SwapFree").value_or(0);
        out.swap_used_bytes = swap_total.value() > swap_free ? swap_total.value() - swap_free : 0;
    }

    return out;
}

constexpr bool k_has_cpu_load = true;
constexpr bool k_has_memory_usage = true;
} // namespace
} // namespace cc

// =========================================================================================================
// Every other target
// =========================================================================================================
#else

namespace cc
{
namespace
{
cc::result<cc::cpu_counter_set, cc::query_error> read_counters()
{
    return cc::error(unsupported_here());
}

cc::result<cc::memory_usage, cc::query_error> read_memory()
{
    return cc::error(unsupported_here());
}

constexpr bool k_has_cpu_load = false;
constexpr bool k_has_memory_usage = false;
} // namespace
} // namespace cc

#endif

// =========================================================================================================
// Shared
// =========================================================================================================

f32 cc::memory_usage::used_ratio() const
{
    if (total_bytes <= 0)
        return 0;
    return f32(f64(used_bytes) / f64(total_bytes));
}

cc::result<cc::cpu_counter_set, cc::query_error> cc::read_cpu_counters()
{
    return cc::read_counters();
}

cc::result<cc::memory_usage, cc::query_error> cc::query_memory_usage()
{
    return cc::read_memory();
}

bool cc::is_metric_supported(cc::metric m)
{
    switch (m)
    {
    case cc::metric::cpu_load:
        return cc::k_has_cpu_load;
    case cc::metric::memory_usage:
        return cc::k_has_memory_usage;
    }
    return false;
}

cc::cpu_load_sampler::cpu_load_sampler()
{
    auto baseline = cc::read_cpu_counters();
    if (baseline.has_value())
    {
        _previous = cc::move(baseline.value());
        _previous_time_secs = cc::current_time_steady_secs();
        _has_baseline = true;
    }
}

bool cc::cpu_load_sampler::is_supported()
{
    return cc::k_has_cpu_load;
}

cc::result<cc::cpu_load, cc::query_error> cc::cpu_load_sampler::sample()
{
    auto current = cc::read_cpu_counters();
    if (current.has_error())
        return cc::error(cc::move(current.error()));

    auto const now = cc::current_time_steady_secs();

    if (!_has_baseline)
    {
        // The constructor could not read a baseline, so this call becomes it and there is nothing to report yet.
        _previous = cc::move(current.value());
        _previous_time_secs = now;
        _has_baseline = true;
        return cc::error(cc::query_error{.status = cc::query_status::failed,
                                         .detail = cc::string("no baseline yet; this call took one")});
    }

    auto& next = current.value();

    _per_core.clear();
    auto const cores
        = next.per_core.size() < _previous.per_core.size() ? next.per_core.size() : _previous.per_core.size();
    for (isize i = 0; i < cores; ++i)
        _per_core.push_back(cc::busy_ratio(_previous.per_core[i], next.per_core[i]));

    auto out = cc::cpu_load();
    out.interval_secs = now - _previous_time_secs;
    out.total = cc::busy_ratio(_previous.total, next.total);
    out.per_core = _per_core;

    _previous = cc::move(next);
    _previous_time_secs = now;

    return out;
}
