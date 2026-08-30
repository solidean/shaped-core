#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/platform/process_metrics.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>
#include <psapi.h>    // GetProcessMemoryInfo, the working set and its peak
#include <tlhelp32.h> // the thread snapshot, which is the only unprivileged thread count

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <mach/mach.h>
#include <mach/task.h>

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <unistd.h>

#endif

namespace cc
{
namespace
{
/// Widening to other processes is a permission story that has not been written yet, so it is reported as a missing
/// capability rather than asserted on.
cc::query_error only_current_process()
{
    return {.status = cc::query_status::unsupported, .detail = cc::string("only cc::current_process is supported today")};
}

cc::query_error process_read_failed(cc::string_view what)
{
    return {.status = cc::query_status::failed, .detail = cc::format("could not read {}", what)};
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
f64 process_filetime_secs(FILETIME const& t)
{
    auto const ticks = (u64(t.dwHighDateTime) << 32) | u64(t.dwLowDateTime);
    return f64(ticks) * 1e-7;
}

/// The only unprivileged route to a thread count is walking the system-wide thread snapshot and counting the ones that
/// belong to us, which is why this is not a field on any of the cheap calls.
cc::optional<i32> count_own_threads()
{
    auto* const snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};

    auto const own = ::GetCurrentProcessId();
    auto entry = THREADENTRY32{};
    entry.dwSize = sizeof(entry);

    auto count = i32(0);
    if (::Thread32First(snapshot, &entry))
        do
        {
            if (entry.dwSize >= FIELD_OFFSET(THREADENTRY32, th32ThreadID) + sizeof(entry.th32OwnerProcessID)
                && entry.th32OwnerProcessID == own)
                ++count;
            entry.dwSize = sizeof(entry);
        } while (::Thread32Next(snapshot, &entry));

    ::CloseHandle(snapshot);
    return count;
}

cc::result<cc::process_usage, cc::query_error> read_usage()
{
    auto counters = PROCESS_MEMORY_COUNTERS_EX{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)))
        return cc::error(process_read_failed("GetProcessMemoryInfo"));

    auto out = cc::process_usage();
    out.resident_bytes = i64(counters.WorkingSetSize);
    out.peak_resident_bytes = i64(counters.PeakWorkingSetSize);
    out.private_bytes = i64(counters.PrivateUsage); // "Commit size" in Task Manager

    DWORD handles = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &handles))
        out.open_handles = i32(handles);

    out.thread_count = count_own_threads().value_or(0);
    return out;
}

cc::result<cc::process_cpu_counters, cc::query_error> read_cpu()
{
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return cc::error(process_read_failed("GetProcessTimes"));

    auto out = cc::process_cpu_counters();
    out.user_secs = process_filetime_secs(user);

    // Unlike GetSystemTimes, a process's kernel time excludes idle: a process is never idle on the CPU's behalf.
    out.kernel_secs = process_filetime_secs(kernel);

    auto io = IO_COUNTERS{};
    if (::GetProcessIoCounters(::GetCurrentProcess(), &io))
    {
        out.bytes_read = i64(io.ReadTransferCount);
        out.bytes_written = i64(io.WriteTransferCount);
    }

    auto memory = PROCESS_MEMORY_COUNTERS{};
    memory.cb = sizeof(memory);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &memory, sizeof(memory)))
        out.page_faults = i64(memory.PageFaultCount);

    return out;
}

constexpr bool k_has_process_metrics = true;
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
cc::result<cc::process_usage, cc::query_error> read_usage()
{
    auto info = mach_task_basic_info_data_t{};
    auto count = mach_msg_type_number_t(MACH_TASK_BASIC_INFO_COUNT);
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS)
        return cc::error(process_read_failed("task_info(MACH_TASK_BASIC_INFO)"));

    auto out = cc::process_usage();
    out.resident_bytes = i64(info.resident_size);
    out.peak_resident_bytes = i64(info.resident_size_max);

    // phys_footprint is what Activity Monitor shows as a process's memory, and virtual_size is the reserved address
    // space — which on Darwin is enormous and means nothing.
    auto vm = task_vm_info_data_t{};
    auto vm_count = mach_msg_type_number_t(TASK_VM_INFO_COUNT);
    if (::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &vm_count) == KERN_SUCCESS)
        out.private_bytes = i64(vm.phys_footprint);
    else
        out.private_bytes = out.resident_bytes;

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (::task_threads(::mach_task_self(), &threads, &thread_count) == KERN_SUCCESS)
    {
        out.thread_count = i32(thread_count);
        for (mach_msg_type_number_t i = 0; i < thread_count; ++i)
            ::mach_port_deallocate(::mach_task_self(), threads[i]);
        ::vm_deallocate(::mach_task_self(), vm_address_t(threads), thread_count * sizeof(thread_act_t));
    }

    return out;
}

cc::result<cc::process_cpu_counters, cc::query_error> read_cpu()
{
    auto info = mach_task_basic_info_data_t{};
    auto count = mach_msg_type_number_t(MACH_TASK_BASIC_INFO_COUNT);
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS)
        return cc::error(process_read_failed("task_info(MACH_TASK_BASIC_INFO)"));

    auto out = cc::process_cpu_counters();
    out.user_secs = f64(info.user_time.seconds) + f64(info.user_time.microseconds) * 1e-6;
    out.kernel_secs = f64(info.system_time.seconds) + f64(info.system_time.microseconds) * 1e-6;

    // MACH_TASK_BASIC_INFO covers terminated threads only; the live ones are a second call.
    auto times = task_thread_times_info_data_t{};
    auto times_count = mach_msg_type_number_t(TASK_THREAD_TIMES_INFO_COUNT);
    if (::task_info(::mach_task_self(), TASK_THREAD_TIMES_INFO, reinterpret_cast<task_info_t>(&times), &times_count)
        == KERN_SUCCESS)
    {
        out.user_secs += f64(times.user_time.seconds) + f64(times.user_time.microseconds) * 1e-6;
        out.kernel_secs += f64(times.system_time.seconds) + f64(times.system_time.microseconds) * 1e-6;
    }

    return out;
}

constexpr bool k_has_process_metrics = true;
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
/// One "VmRSS:   12345 kB" style field of /proc/self/status, in bytes.
cc::optional<i64> status_kib(cc::string_view text, cc::string_view key)
{
    auto const field = cc::impl::field_from(text, key, ':');
    if (!field.has_value())
        return {};

    auto number = cc::string_view(field.value());
    if (auto const space = number.find(' '); space >= 0)
        number = number.subview_clamped(0, space);

    auto const value = cc::from_string<i64>(number);
    if (!value.has_value())
        return {};
    return value.value() * 1024;
}

cc::result<cc::process_usage, cc::query_error> read_usage()
{
    auto const text = cc::impl::read_text_file("/proc/self/status");
    if (!text.has_value())
        return cc::error(process_read_failed("/proc/self/status"));

    auto out = cc::process_usage();
    out.resident_bytes = status_kib(text.value(), "VmRSS").value_or(0);

    // VmHWM is the kernel's own high-water mark, which is exactly the peak no sampling could reconstruct.
    out.peak_resident_bytes = status_kib(text.value(), "VmHWM").value_or(out.resident_bytes);
    // RssAnon is the anonymous memory this process actually holds, and VmSwap the part of it pushed out to swap.
    // NOT VmData, which is private address space whether or not a page was ever touched — mimalloc's arena reservation
    // alone puts about a gigabyte in it, which is the reserved-and-untouched figure this field promises not to be.
    out.private_bytes = status_kib(text.value(), "RssAnon").value_or(0) + status_kib(text.value(), "VmSwap").value_or(0);

    if (auto const threads = cc::impl::field_from(text.value(), "Threads", ':'); threads.has_value())
        if (auto const value = cc::from_string<i64>(cc::string_view(threads.value())); value.has_value())
            out.thread_count = i32(value.value());

    // The open descriptor count would mean listing /proc/self/fd, and cc has no directory walk.
    // FDSize is NOT it: that is how many slots the table has room for, which is a different and much larger number.

    return out;
}

cc::result<cc::process_cpu_counters, cc::query_error> read_cpu()
{
    auto const text = cc::impl::read_text_file("/proc/self/stat");
    if (!text.has_value())
        return cc::error(process_read_failed("/proc/self/stat"));

    // Field 2 is the executable name in parentheses and may itself contain spaces, so the fields are counted from the
    // closing parenthesis rather than from the start of the line.
    auto const line = cc::string_view(text.value());
    auto const close = line.rfind(')');
    if (close < 0)
        return cc::error(process_read_failed("/proc/self/stat: malformed"));

    auto rest = cc::impl::trimmed(line.subview(close + 1));

    auto const clock = ::sysconf(_SC_CLK_TCK);
    auto const per_tick = clock > 0 ? 1.0 / f64(clock) : 1.0 / 100.0;

    // The closing parenthesis ends field 2, so `rest` starts at field 3 — the run state — and index n here is
    // documented field n + 2.
    // minflt is field 10, utime is 14 and stime is 15.
    auto out = cc::process_cpu_counters();
    for (auto index = 1; index <= 13 && !rest.empty(); ++index)
    {
        auto const next = rest.find(' ');
        auto const token = next < 0 ? rest : rest.subview_clamped(0, next);
        rest = next < 0 ? cc::string_view() : cc::impl::trimmed(rest.subview(next));

        auto const value = cc::from_string<i64>(token);
        if (!value.has_value())
            continue;

        if (index == 8)
            out.page_faults = value.value(); // minflt, which is the count Windows' PageFaultCount is closest to
        else if (index == 12)
            out.user_secs = f64(value.value()) * per_tick;
        else if (index == 13)
            out.kernel_secs = f64(value.value()) * per_tick;
    }

    // /proc/self/io is unreadable under some hardening configurations, so its absence is normal rather than an error.
    if (auto const io = cc::impl::read_text_file("/proc/self/io"); io.has_value())
    {
        if (auto const field = cc::impl::field_from(io.value(), "rchar", ':'); field.has_value())
            out.bytes_read = cc::from_string<i64>(cc::string_view(field.value()));
        if (auto const field = cc::impl::field_from(io.value(), "wchar", ':'); field.has_value())
            out.bytes_written = cc::from_string<i64>(cc::string_view(field.value()));
    }

    return out;
}

constexpr bool k_has_process_metrics = true;
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
cc::result<cc::process_usage, cc::query_error> read_usage()
{
    return cc::error(
        cc::query_error{.status = cc::query_status::unsupported, .detail = cc::string("no process accounting here")});
}

cc::result<cc::process_cpu_counters, cc::query_error> read_cpu()
{
    return cc::error(
        cc::query_error{.status = cc::query_status::unsupported, .detail = cc::string("no process accounting here")});
}

constexpr bool k_has_process_metrics = false;
} // namespace
} // namespace cc

#endif

// =========================================================================================================
// Shared
// =========================================================================================================

cc::result<cc::process_usage, cc::query_error> cc::query_process_usage(cc::process_id id)
{
    if (id != cc::current_process)
        return cc::error(cc::only_current_process());
    return cc::read_usage();
}

cc::result<cc::process_cpu_counters, cc::query_error> cc::read_process_cpu_counters(cc::process_id id)
{
    if (id != cc::current_process)
        return cc::error(cc::only_current_process());
    return cc::read_cpu();
}

bool cc::process_cpu_sampler::is_supported()
{
    return cc::k_has_process_metrics;
}

cc::process_cpu_sampler::process_cpu_sampler(cc::process_id id) : _id(id)
{
    auto baseline = cc::read_process_cpu_counters(id);
    if (baseline.has_value())
    {
        _previous = cc::move(baseline.value());
        _previous_time_secs = cc::current_time_steady_secs();
        _has_baseline = true;
    }
}

cc::result<cc::process_cpu_load, cc::query_error> cc::process_cpu_sampler::sample()
{
    auto current = cc::read_process_cpu_counters(_id);
    if (current.has_error())
        return cc::error(cc::move(current.error()));

    auto const now = cc::current_time_steady_secs();

    if (!_has_baseline)
    {
        _previous = cc::move(current.value());
        _previous_time_secs = now;
        _has_baseline = true;
        return cc::error(cc::query_error{.status = cc::query_status::failed,
                                         .detail = cc::string("no baseline yet; this call took one")});
    }

    auto const interval = now - _previous_time_secs;
    auto const used = current.value().total_secs() - _previous.total_secs();

    auto out = cc::process_cpu_load();
    out.interval_secs = interval;
    out.cores_used = interval > 0 && used > 0 ? f32(used / interval) : 0.0f;

    if (auto const cores = cc::get_system_info().logical_cores(); cores > 0)
    {
        // Clamped to the machine, for the same reason the machine's own load is clamped, and it is not cosmetic.
        // Process CPU time is quantized far more coarsely than the steady clock — 15.6 ms on Windows — so a short
        // interval divides a rounded-up numerator by an exact denominator and reports more cores than the machine has.
        // Seen at 33.4 on a 32-core box over a 20 ms sample.
        if (out.cores_used > f32(cores))
            out.cores_used = f32(cores);

        out.machine_fraction = out.cores_used / f32(cores);
    }

    _previous = cc::move(current.value());
    _previous_time_secs = now;

    return out;
}
