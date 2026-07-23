#include <clean-core/container/fixed_vector.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <evntcons.h>
#include <evntrace.h>
#include <nexus/bench/impl/baseline.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

#include <atomic>

// The PMC counter values ETW attaches to an event live in an extended-data item of this type; the payload is
// an array of ULONG64, one per configured source, in the order the source list was set. Older SDKs omit the
// macro even though the runtime emits it.
#ifndef EVENT_HEADER_EXT_TYPE_PMC_COUNTERS
#define EVENT_HEADER_EXT_TYPE_PMC_COUNTERS 0x0008
#endif

// Windows backend.
//
// Always-on baseline: elapsed time and, on x86, an rdtsc reference-cycle count — works with no privileges
// anywhere, including the virtualized CI runners where the PMU is hidden.
//
// Full PMU counters use the ETW profile sources + a real-time context-switch consumer:
//   - TraceQueryInformation(TraceProfileSourceListInfo) enumerates the CPU's named PMU sources; we map our
//     logical counters onto them by name (names differ per CPU/Windows version, and that is fine).
//   - We start our own private SystemTraceProvider session (a fixed GUID, EVENT_TRACE_SYSTEM_LOGGER_MODE),
//     enable CSWITCH, and attach the chosen PMC sources with two calls: TracePmcCounterListInfo picks the
//     counters, TracePmcEventListInfo stamps them onto context-switch events. ETW then tags every CSwitch
//     with that CPU's PMC counter values.
//   - A background thread runs ProcessTrace and, for each CSwitch, tracks our benchmark thread across all of
//     its scheduling intervals: on switch-in it snapshots that CPU's counters, on switch-out it adds the
//     delta. Summing every interval the thread ran gives the counts for the measured region — multi-quantum
//     and CPU migration both fall out for free (each interval is bracketed by a switch-in/out on one CPU).
//   - Because a short loop can run entirely inside one quantum (no context switch during it), measure()
//     forces a switch just before and just after body() so the region is bracketed by real CSwitch events,
//     and bounds counting to that time window.
//
// Reading counters this way needs a non-admin to be able to start the session: run tools/setup-pmu-access.ps1
// once (Performance Log Users + SeSystemProfilePrivilege + EventAccessControl DACLs on the session and
// System-Trace-Provider GUIDs). Without that StartTrace returns ERROR_ACCESS_DENIED and we degrade to the
// baseline. The legacy ReadThreadProfilingData hardware-counter path is deliberately NOT used: it reads back
// all zeros in user mode on every machine tried (its HwCountersCount is gated on a global PMC list that
// cannot be set from user mode).
//
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

bool is_baseline(hw_counter c)
{
    return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles;
}

constexpr char const* s_setup_hint = "run tools/setup-pmu-access.ps1 (elevated, once) to grant non-admin PMU "
                                     "access — Performance Log Users, SeSystemProfilePrivilege, and the ETW "
                                     "session-GUID ACLs — then sign out and back in, or run elevated.";

// Private logger name and fixed session GUID for our SystemTraceProvider session.
// A non-admin process may start this session only if setup-pmu-access.ps1 has ACL'd the user's SID onto this
// exact GUID (EventAccessControl) — the GUID here and in the script MUST stay identical.
constexpr char const* s_session_name = "nexus-bench-pmu";
constexpr GUID s_session_guid = {0x6b3c9a10, 0x2f4d, 0x4e8a, {0x9c, 0x1b, 0x7d, 0x5e, 0x3a, 0x2f, 0x8b, 0x60}};

// Kernel Thread-provider GUID and its context-switch opcode: the event PMC counters are stamped onto.
constexpr GUID s_thread_guid = {0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr u8 s_cswitch_opcode = 36;

// A properties buffer big enough for EVENT_TRACE_PROPERTIES plus the trailing logger name.
constexpr auto s_props_size = isize(sizeof(EVENT_TRACE_PROPERTIES) + 256);

char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

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

// One PMU source the current CPU exposes: the ETW profile-source id and its native name.
struct profile_source
{
    u32 id;
    cc::string name;
};

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
        // Claimed before cache_llc_misses, so the L1 D-cache source is taken here rather than by the generic
        // "CacheMiss" spelling below.
        return icontains(name, "DcacheMiss") || icontains(name, "L1DMiss") || icontains(name, "L1DCacheMiss")
            || icontains(name, "DataCacheMiss");
    case hw_counter::cache_llc_references:
        return icontains(name, "LLCReference") || icontains(name, "LLCRef") || icontains(name, "DcacheAccess")
            || icontains(name, "DCAccess");
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
    }
    ::CloseHandle(token);
}

// Fill a zeroed EVENT_TRACE_PROPERTIES for our private real-time system logger. `buffer` must be s_props_size.
EVENT_TRACE_PROPERTIES* init_session_props(unsigned char* buffer)
{
    for (auto i = isize(0); i < s_props_size; ++i)
        buffer[i] = 0;
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer);
    p->Wnode.BufferSize = ULONG(s_props_size);
    p->Wnode.Guid = s_session_guid; // the ACL'd GUID that lets a non-admin start this system logger
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 2; // system-time (FILETIME) timestamps — same clock as the measurement window
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return p;
}

// Start our session, replacing any stale instance left by a crashed run. `props` must be s_props_size.
ULONG start_session(TRACEHANDLE& session, unsigned char* props)
{
    auto status = ::StartTraceA(&session, s_session_name, init_session_props(props));
    if (status == ERROR_ALREADY_EXISTS)
    {
        ::ControlTraceA(0, s_session_name, init_session_props(props), EVENT_TRACE_CONTROL_STOP);
        status = ::StartTraceA(&session, s_session_name, init_session_props(props));
    }
    return status;
}

// Attach the PMC counters to context-switch events. Both TraceSetInformation calls are required: without the
// event-list call the counters are configured but attached to no event, so every CSwitch arrives with no PMC
// extended-data item. Returns true when both succeed.
bool configure_session_pmc(TRACEHANDLE session, ULONG const* source_ids, isize count)
{
    auto flags = ULONG(EVENT_TRACE_FLAG_CSWITCH);
    ::TraceSetInformation(session, TraceSystemTraceEnableFlagsInfo, &flags, sizeof(flags));

    auto const pmc_status = ::TraceSetInformation(session, TracePmcCounterListInfo, const_cast<ULONG*>(source_ids),
                                                  ULONG(count * sizeof(ULONG)));
    auto pmc_events = CLASSIC_EVENT_ID{};
    pmc_events.EventGuid = s_thread_guid;
    pmc_events.Type = s_cswitch_opcode;
    auto const evt_status = ::TraceSetInformation(session, TracePmcEventListInfo, &pmc_events, sizeof(pmc_events));

    return pmc_status == ERROR_SUCCESS && evt_status == ERROR_SUCCESS;
}

// Can this process actually read PMU counters right now? Probed once by really starting the session, then
// stopping it — the honest test of the privilege + ACL setup, which is the whole gate (StartTrace returns
// ERROR_ACCESS_DENIED without it). Deliberately does NOT bind PMC sources: that would leave configuration
// residue that makes the next real measure's TracePmcCounterListInfo fail. Cached: the answer is stable for
// the process (re-running setup-pmu-access.ps1 needs a fresh process to take effect here).
bool pmu_readable()
{
    static auto const readable = []
    {
        if (profile_sources().empty())
            return false;

        enable_system_profile_privilege();

        alignas(8) unsigned char props[s_props_size] = {};
        auto session = TRACEHANDLE(0);
        if (start_session(session, props) != ERROR_SUCCESS)
            return false;

        ::ControlTraceA(session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props), EVENT_TRACE_CONTROL_STOP);
        return true;
    }();
    return readable;
}

// Shared state between the measuring thread and the real-time consumer thread.
// The consumer is the only writer of totals/last_in/running; the measuring thread reads totals only after the
// consumer has stopped and been joined, so no locking is needed there. t_arm / t_disarm bound the counted
// window and are read live by the consumer, so they are atomic.
struct pmc_state
{
    // Logical processors we track per-CPU counter snapshots for. Events on a higher-numbered CPU are ignored
    // (that interval goes uncounted) — only reachable on >256-way machines.
    static constexpr int max_cpus = 256;

    u32 target_tid = 0;
    isize counter_count = 0;
    std::atomic<i64> t_arm{0};
    std::atomic<i64> t_disarm{INT64_MAX};

    bool running[max_cpus] = {};
    u64 last_in[max_cpus][MAX_HW_COUNTERS] = {};
    u64 totals[MAX_HW_COUNTERS] = {};
};

// ProcessTrace callback: one call per delivered event. We care only about context switches that carry PMC
// extended data, and only while our benchmark thread is the one being switched in/out.
void WINAPI on_event(EVENT_RECORD* rec)
{
    auto* st = static_cast<pmc_state*>(rec->UserContext);
    if (st == nullptr || rec->EventHeader.EventDescriptor.Opcode != s_cswitch_opcode)
        return;

    // The PMC counter values ride along as an extended-data item.
    u64 const* pmc = nullptr;
    auto pmc_count = isize(0);
    for (auto i = u16(0); i < rec->ExtendedDataCount; ++i)
        if (rec->ExtendedData[i].ExtType == EVENT_HEADER_EXT_TYPE_PMC_COUNTERS)
        {
            pmc = reinterpret_cast<u64 const*>(static_cast<uintptr_t>(rec->ExtendedData[i].DataPtr));
            pmc_count = isize(rec->ExtendedData[i].DataSize / sizeof(u64));
            break;
        }
    if (pmc == nullptr)
        return;

    auto const ts = i64(rec->EventHeader.TimeStamp.QuadPart);
    if (ts < st->t_arm.load(std::memory_order_relaxed) || ts > st->t_disarm.load(std::memory_order_relaxed))
        return;

    // CSwitch payload begins with NewThreadId then OldThreadId (stable kernel MOF layout).
    if (rec->UserDataLength < 8)
        return;
    auto const* ids = static_cast<u32 const*>(rec->UserData);
    auto const new_tid = ids[0];
    auto const old_tid = ids[1];
    auto const cpu = int(rec->BufferContext.ProcessorIndex);
    if (cpu < 0 || cpu >= pmc_state::max_cpus)
        return;

    auto const n = pmc_count < st->counter_count ? pmc_count : st->counter_count;

    // Our thread ran from its switch-in on this CPU until now: fold in the per-CPU delta.
    if (old_tid == st->target_tid && st->running[cpu])
    {
        for (auto i = isize(0); i < n; ++i)
            st->totals[i] += pmc[i] - st->last_in[cpu][i];
        st->running[cpu] = false;
    }
    // Our thread starts running on this CPU: snapshot the per-CPU counters.
    if (new_tid == st->target_tid)
    {
        for (auto i = isize(0); i < n; ++i)
            st->last_in[cpu][i] = pmc[i];
        st->running[cpu] = true;
    }
}

DWORD WINAPI consumer_proc(void* ctx)
{
    auto handle = *static_cast<TRACEHANDLE*>(ctx);
    ::ProcessTrace(&handle, 1, nullptr, nullptr); // blocks until the session is stopped and buffers drained
    return 0;
}

// Force a real context switch: an unsignaled wait with a tiny timeout deschedules this thread and lets the
// scheduler run something else, producing the CSwitch events that bracket our region.
void force_context_switch()
{
    static auto const ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset, never signaled
    ::WaitForSingleObject(ev, 1);
}
} // namespace

cc::vector<backend_counter> backend_enumerate_counters()
{
    cc::vector<backend_counter> out;
    out.push_back({.id = hw_counter::elapsed_nanoseconds, .available = true});
    out.push_back({.id = hw_counter::reference_cycles, .available = has_reference_cycles()});

    // A PMU counter is measurable only if its source exists AND we can actually start the session to read it.
    auto const readable = pmu_readable();
    auto used = cc::vector<bool>::create_defaulted(profile_sources().size());
    for (auto const c : s_pmu_counters)
    {
        auto const idx = find_source(c, used);
        if (idx >= 0)
        {
            used[idx] = true;
            out.push_back({.id = c, .native_name = profile_sources()[idx].name, .available = readable});
        }
        else
            out.push_back({.id = c, .available = false});
    }
    return out;
}

cc::string backend_setup_hint()
{
    if (!profile_sources().empty() && !pmu_readable())
        return cc::string(s_setup_hint);
    return {};
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

    // ETW session + consumer state.
    auto state = pmc_state{};
    state.target_tid = ::GetCurrentThreadId();

    auto trace_session = TRACEHANDLE(0);
    auto consumer_handle = TRACEHANDLE(0);
    auto consumer_thread = HANDLE(nullptr);
    auto counters_live = false;
    auto configured = isize(0); // how many of `mapped` we actually programmed (PMC counters are limited)
    alignas(8) unsigned char session_props[s_props_size] = {};

    if (!mapped.empty())
    {
        enable_system_profile_privilege();

        ULONG source_ids[MAX_HW_COUNTERS] = {};
        for (auto i = isize(0); i < mapped.size(); ++i)
            source_ids[i] = mapped[i].source;

        // Only a limited number of hardware PMC counters can be programmed at once (a few per core, minus
        // whatever other ETW sessions currently hold), so configuring all requested sources can fail with
        // ERROR_BUSY. Degrade to as many as fit right now: drop the last-requested source and retry, so a
        // caller always gets its highest-priority counters rather than nothing. Each attempt needs a fresh
        // session — a session whose PMC configuration was rejected once will not accept a smaller one.
        for (auto want = mapped.size(); want >= 1; --want)
        {
            if (start_session(trace_session, session_props) == ERROR_SUCCESS
                && configure_session_pmc(trace_session, source_ids, want))
            {
                configured = want;
                break;
            }
            if (trace_session != 0)
            {
                ::ControlTraceA(trace_session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(session_props),
                                EVENT_TRACE_CONTROL_STOP);
                trace_session = 0;
            }
        }

        if (configured > 0)
        {
            state.counter_count = configured; // the consumer reads exactly this many PMC values per event

            auto logfile = EVENT_TRACE_LOGFILEA{};
            logfile.LoggerName = const_cast<char*>(s_session_name);
            logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
            logfile.EventRecordCallback = &on_event;
            logfile.Context = &state;

            consumer_handle = ::OpenTraceA(&logfile);
            if (consumer_handle != INVALID_PROCESSTRACE_HANDLE)
            {
                consumer_thread = ::CreateThread(nullptr, 0, &consumer_proc, &consumer_handle, 0, nullptr);
                counters_live = consumer_thread != nullptr;
            }
        }
    }

    // Arm the counted window, bracket the single body() call with forced context switches, and read the
    // baseline as tightly as possible around it. The window clock must match the event timestamps (FILETIME).
    auto now_ts = [&]
    {
        auto ft = FILETIME{};
        ::GetSystemTimePreciseAsFileTime(&ft);
        return i64((u64(ft.dwHighDateTime) << 32) | u64(ft.dwLowDateTime));
    };

    if (counters_live)
    {
        state.t_arm.store(now_ts(), std::memory_order_relaxed);
        force_context_switch(); // switch out + back in -> a switch-in event snapshots the start counters
    }

    auto const cycles_begin = read_reference_cycles();
    auto const ns_begin = steady_now_ns();
    body();
    auto const ns_end = steady_now_ns();
    auto const cycles_end = read_reference_cycles();

    if (counters_live)
    {
        force_context_switch(); // switch out -> a switch-out event folds in the region delta
        state.t_disarm.store(now_ts(), std::memory_order_relaxed);
    }

    // Stop the session (flushes buffers), drain the consumer, then read the accumulated totals.
    if (trace_session != 0)
        ::ControlTraceA(trace_session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(session_props),
                        EVENT_TRACE_CONTROL_STOP);
    if (consumer_thread != nullptr)
    {
        ::WaitForSingleObject(consumer_thread, INFINITE);
        ::CloseHandle(consumer_thread);
    }
    if (consumer_handle != 0 && consumer_handle != INVALID_PROCESSTRACE_HANDLE)
        ::CloseTrace(consumer_handle);

    if (pmu_requested && !counters_live)
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

        // A PMU counter: its value sits at the same index in state.totals as in our mapped list. Only the
        // first `configured` sources were actually programmed (the rest lost the PMC-counter budget race).
        auto value = u64(0);
        auto valid = false;
        for (auto i = isize(0); i < mapped.size(); ++i)
        {
            if (mapped[i].id != c)
                continue;
            name = mapped[i].name;
            if (counters_live && i < configured)
            {
                value = state.totals[i];
                valid = true;
            }
            break;
        }
        out.push_back({.id = c, .name = name, .value = value, .valid = valid});
    }
    return out;
}
} // namespace nx::bench::impl
