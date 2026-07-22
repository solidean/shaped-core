#include <nexus/bench/impl/baseline.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/string/print.hh>

#include <clean-core/platform/win32_sanitized.hh>

#include <evntrace.h>
#include <realtimeapiset.h>

#ifndef READ_THREAD_PROFILING_FLAG_HARDWARE_COUNTERS
#define READ_THREAD_PROFILING_FLAG_HARDWARE_COUNTERS 0x00000002
#endif

// Windows backend.
//
// Always-on baseline: elapsed time and, on x86, an rdtsc reference-cycle count — works with no privileges
// anywhere, including the virtualized CI runners where the PMU is hidden.
//
// Full PMU counters use the ETW profile sources + the Thread Profiling API:
//   - TraceQueryInformation(TraceProfileSourceListInfo) enumerates the CPU's named PMU sources; we map our
//     logical counters onto them by name (names differ per CPU/Windows version, and that is fine).
//   - TraceSetInformation(TracePmcCounterListInfo) selects the sources to collect (needs
//     SeSystemProfilePrivilege — grant it once with tools/setup-pmu-access.ps1, or run elevated).
//   - EnableThreadProfiling / ReadThreadProfilingData / DisableThreadProfiling bracket the single body()
//     call and return the per-thread counter deltas.
// Anything unavailable (no privilege, virtualized, unsupported source) degrades to the baseline and warns
// once.

namespace nx::bench::impl
{
namespace
{
// The PMU counters this backend maps onto ETW profile sources (everything except the baseline pair).
constexpr hw_counter s_pmu_counters[] = {
    hw_counter::instructions_retired, hw_counter::branch_instructions,  hw_counter::branch_misses,
    hw_counter::cache_l1d_misses,     hw_counter::cache_llc_references, hw_counter::cache_llc_misses,
};

bool is_baseline(hw_counter c) { return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles; }

constexpr char const* s_setup_hint = "run tools/setup-pmu-access.ps1 to grant the standard PMU permissions "
                                     "(SeSystemProfilePrivilege), then sign out and back in — or run elevated.";

// One PMU source the current CPU exposes: the ETW profile-source id and its native name.
struct profile_source
{
    u32 id;
    cc::string name;
};

char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool icontains(cc::string const& hay, cc::string_view needle)
{
    auto const n = needle.size();
    if (n == 0)
        return true;
    auto const h = hay.size();
    for (auto i = isize(0); i + n <= h; ++i)
    {
        auto match = true;
        for (auto j = isize(0); j < n; ++j)
            if (to_lower(hay[i + j]) != to_lower(needle[j]))
            {
                match = false;
                break;
            }
        if (match)
            return true;
    }
    return false;
}

// Narrow an ASCII-ish wide string (a profile-source name) to cc::string.
cc::string narrow(wchar_t const* w)
{
    if (w == nullptr)
        return {};
    auto const needed = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    auto buffer = cc::vector<char>::create_defaulted(needed);
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, buffer.data(), needed, nullptr, nullptr);
    return cc::string(cc::string_view(buffer.data(), needed - 1)); // drop the trailing null
}

// Enumerate the CPU's PMU profile sources once (the list is stable for the process lifetime).
cc::vector<profile_source> const& profile_sources()
{
    static auto const sources = []
    {
        cc::vector<profile_source> out;
        alignas(8) unsigned char buffer[8192] = {};
        auto return_length = ULONG(0);
        auto const status
            = ::TraceQueryInformation(0, TraceProfileSourceListInfo, buffer, ULONG(sizeof(buffer)), &return_length);
        if (status != ERROR_SUCCESS)
            return out;

        auto const* info = reinterpret_cast<PROFILE_SOURCE_INFO const*>(buffer);
        while (true)
        {
            out.push_back({.id = u32(info->Source), .name = narrow(info->Description)});
            if (info->NextEntryOffset == 0)
                break;
            info = reinterpret_cast<PROFILE_SOURCE_INFO const*>(reinterpret_cast<unsigned char const*>(info)
                                                                + info->NextEntryOffset);
        }
        return out;
    }();
    return sources;
}

// Does an ETW profile-source name look like the given logical counter? Names are CPU-specific, so match a
// few known spellings; the greedy assignment below keeps one source from being claimed twice.
bool source_matches(hw_counter c, cc::string const& name)
{
    switch (c)
    {
    case hw_counter::instructions_retired:
        return icontains(name, "InstructionRetired") || icontains(name, "InstructionsRetired")
               || icontains(name, "TotalIssues");
    case hw_counter::branch_instructions:
        return icontains(name, "BranchInstruction");
    case hw_counter::branch_misses:
        return icontains(name, "BranchMispredict");
    case hw_counter::cache_l1d_misses:
        return icontains(name, "L1DMiss") || icontains(name, "L1DCacheMiss") || icontains(name, "DataCacheMiss");
    case hw_counter::cache_llc_references:
        return icontains(name, "LLCReference") || icontains(name, "LLCRef");
    case hw_counter::cache_llc_misses:
        return icontains(name, "LLCMiss") || icontains(name, "CacheMiss");
    default:
        return false;
    }
}

// Index of the first not-yet-claimed profile source matching `c`, or -1.
isize find_source(hw_counter c, cc::span<bool> used)
{
    auto const& sources = profile_sources();
    for (auto i = isize(0); i < sources.size(); ++i)
        if (!used[i] && source_matches(c, sources[i].name))
            return i;
    return -1;
}

// Enable SeSystemProfilePrivilege in this process token (held is not enough; it must be enabled).
void enable_system_profile_privilege()
{
    auto token = HANDLE(nullptr);
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;

    auto luid = LUID{};
    if (::LookupPrivilegeValueA(nullptr, SE_SYSTEM_PROFILE_NAME, &luid))
    {
        auto tp = TOKEN_PRIVILEGES{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        cc::eprintln("[diag] AdjustTokenPrivileges GetLastError={} (0=ok, 1300=not-all-assigned)", u32(::GetLastError()));
    }
    ::CloseHandle(token);
}
} // namespace

cc::vector<backend_counter> backend_enumerate_counters()
{
    cc::vector<backend_counter> out;
    out.push_back({.id = hw_counter::elapsed_nanoseconds, .available = true});
    out.push_back({.id = hw_counter::reference_cycles, .available = has_reference_cycles()});

    auto used = cc::vector<bool>::create_defaulted(profile_sources().size());
    for (auto const c : s_pmu_counters)
    {
        auto const idx = find_source(c, used);
        if (idx >= 0)
        {
            used[idx] = true;
            out.push_back({.id = c, .native_name = profile_sources()[idx].name, .available = true});
        }
        else
            out.push_back({.id = c, .available = false});
    }
    return out;
}

cc::vector<hw_counter_sample> backend_measure(cc::function_ref<void()> body, cc::span<hw_counter const> counters)
{
    // Map each requested PMU counter to a profile source (in request order, up to the hardware limit).
    struct mapped_counter
    {
        hw_counter id;
        u32 source;
        cc::string name;
    };
    cc::fixed_vector<mapped_counter, MAX_HW_COUNTERS> mapped;

    auto used = cc::vector<bool>::create_defaulted(profile_sources().size());
    auto pmu_requested = false;
    for (auto const c : counters)
    {
        if (is_baseline(c))
            continue;
        pmu_requested = true;
        if (mapped.full())
            continue;

        auto const idx = find_source(c, used);
        if (idx < 0)
            continue;
        used[idx] = true;
        mapped.push_back({.id = c, .source = profile_sources()[idx].id, .name = profile_sources()[idx].name});
    }

    // Configure the PMC sources and enable per-thread profiling.
    auto profiling_handle = HANDLE(nullptr);
    auto profiling_on = false;
    auto trace_session = TRACEHANDLE(0);
    char const* const session_name = "nexus-bench-pmu";
    alignas(8) unsigned char session_props[sizeof(EVENT_TRACE_PROPERTIES) + 256] = {};
    if (!mapped.empty())
    {
        enable_system_profile_privilege();

        ULONG source_ids[MAX_HW_COUNTERS] = {};
        for (auto i = isize(0); i < mapped.size(); ++i)
            source_ids[i] = mapped[i].source;

        // The PMC counter list must attach to a real trace session, so start a real-time system logger.
        auto const init_props = [&]
        {
            for (auto& b : session_props)
                b = 0;
            auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(session_props);
            p->Wnode.BufferSize = ULONG(sizeof(session_props));
            p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            p->Wnode.ClientContext = 1; // QPC timestamps
            p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
            p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            return p;
        };

        auto* props = init_props();
        auto start_status = ::StartTraceA(&trace_session, session_name, props);
        if (start_status == ERROR_ALREADY_EXISTS)
        {
            ::ControlTraceA(0, session_name, init_props(), EVENT_TRACE_CONTROL_STOP);
            props = init_props();
            start_status = ::StartTraceA(&trace_session, session_name, props);
        }
        cc::eprintln("[diag] StartTrace status={}", u32(start_status));

        if (start_status == ERROR_SUCCESS)
        {
            auto flags = ULONG(EVENT_TRACE_FLAG_CSWITCH);
            auto const flags_status = ::TraceSetInformation(trace_session, TraceSystemTraceEnableFlagsInfo, &flags,
                                                            sizeof(flags));
            auto const set_status = ::TraceSetInformation(trace_session, TracePmcCounterListInfo, source_ids,
                                                          ULONG(mapped.size() * sizeof(ULONG)));
            // EnableThreadProfiling may read the GLOBAL pmc list; now that a session exists, try setting it too.
            auto const global_status
                = ::TraceSetInformation(0, TracePmcCounterListInfo, source_ids, ULONG(mapped.size() * sizeof(ULONG)));
            cc::eprintln("[diag] mapped={} flags status={} sessionPmc status={} globalPmc status={}", mapped.size(),
                         u32(flags_status), u32(set_status), u32(global_status));
            if (set_status == ERROR_SUCCESS)
            {
                auto mask = DWORD64(0);
                for (auto i = isize(0); i < mapped.size(); ++i)
                    mask |= DWORD64(1) << i;
                auto const en_status = ::EnableThreadProfiling(::GetCurrentThread(), 0, mask, &profiling_handle);
                cc::eprintln("[diag] EnableThreadProfiling status={}", u32(en_status));
                profiling_on = en_status == ERROR_SUCCESS;
            }
        }
    }

    // Bracket the single invocation with the baseline as tightly as possible.
    auto const cycles_begin = read_reference_cycles();
    auto const ns_begin = steady_now_ns();
    body();
    auto const ns_end = steady_now_ns();
    auto const cycles_end = read_reference_cycles();

    // Read and stop the per-thread counters.
    auto perf_data = PERFORMANCE_DATA{};
    auto read_ok = false;
    if (profiling_on)
    {
        perf_data.Size = sizeof(PERFORMANCE_DATA);
        perf_data.Version = PERFORMANCE_DATA_VERSION;
        read_ok = ::ReadThreadProfilingData(profiling_handle, READ_THREAD_PROFILING_FLAG_HARDWARE_COUNTERS, &perf_data)
                  == ERROR_SUCCESS;
        ::DisableThreadProfiling(profiling_handle);
        cc::eprintln("[diag] read_ok={} HwCountersCount={} ContextSwitchCount={} CycleTime={}", read_ok,
                     u32(perf_data.HwCountersCount), u32(perf_data.ContextSwitchCount), u64(perf_data.CycleTime));
        for (auto i = 0; i < MAX_HW_COUNTERS; ++i)
            cc::eprintln("[diag]   slot {} Type={} Value={}", i, u32(perf_data.HwCounters[i].Type),
                         u64(perf_data.HwCounters[i].Value));
    }

    if (trace_session != 0)
        ::ControlTraceA(trace_session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(session_props),
                        EVENT_TRACE_CONTROL_STOP);

    if (pmu_requested && !read_ok)
        warn_pmu_unavailable_once(s_setup_hint);

    cc::vector<hw_counter_sample> out;
    out.reserve(counters.size());
    for (auto const c : counters)
    {
        auto name = cc::string(logical_counter_name(c));
        if (c == hw_counter::elapsed_nanoseconds)
        {
            out.push_back({.id = c, .name = name, .value = ns_end - ns_begin, .valid = true});
            continue;
        }
        if (c == hw_counter::reference_cycles)
        {
            out.push_back({.id = c, .name = name, .value = cycles_end - cycles_begin, .valid = has_reference_cycles()});
            continue;
        }

        // A PMU counter: its value sits at the same index in perf_data.HwCounters as in our mapped list.
        auto value = u64(0);
        auto valid = false;
        for (auto i = isize(0); i < mapped.size(); ++i)
        {
            if (mapped[i].id != c)
                continue;
            name = mapped[i].name;
            if (read_ok && i < isize(perf_data.HwCountersCount))
            {
                value = u64(perf_data.HwCounters[i].Value);
                valid = true;
            }
            break;
        }
        out.push_back({.id = c, .name = name, .value = value, .valid = valid});
    }
    return out;
}
} // namespace nx::bench::impl
