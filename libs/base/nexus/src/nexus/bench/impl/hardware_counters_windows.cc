#include <clean-core/common/time.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/platform/impl/cpuid.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <evntcons.h>
#include <evntrace.h>
#include <nexus/bench/impl/hardware_counters_backend.hh>

#include <atomic>

// The PMC values ETW attaches to an event live in an extended-data item of this type.
// The payload is an array of ULONG64, one per configured source, in the order the source list was set.
// Older SDKs omit the macro even though the runtime emits it.
#ifndef EVENT_HEADER_EXT_TYPE_PMC_COUNTERS
#define EVENT_HEADER_EXT_TYPE_PMC_COUNTERS 0x0008
#endif

// Windows backend: PMU counters via a real-time ETW context-switch session, over a baseline that always works.
//
// docs/guides/profiling.md owns the mechanism — the profile-source mapping, the two TraceSetInformation calls,
// the consumer thread that sums per-scheduling-interval deltas, and why ReadThreadProfilingData is not used.
// Read it before changing anything here.
//
// Anything unavailable — no privilege, virtualized, unsupported source — degrades to the baseline and warns once.

namespace nx::bench::impl
{
namespace
{
// The PMU counters this backend maps onto ETW profile sources (everything except the baseline pair).
constexpr hw_counter s_pmu_counters[] = {
    hw_counter::instructions_retired, hw_counter::branch_instructions,  hw_counter::branch_misses,
    hw_counter::cache_l1d_misses,     hw_counter::cache_llc_references, hw_counter::cache_llc_misses,
    hw_counter::core_cycles,          hw_counter::cache_l1d_accesses,
};

bool is_baseline(hw_counter c)
{
    return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles;
}

constexpr char const* s_setup_hint = "run tools/setup-pmu-access.ps1 (elevated, once) to grant non-admin PMU "
                                     "access — Performance Log Users, SeSystemProfilePrivilege, and the ETW "
                                     "session-GUID ACLs — then sign out and back in, or run elevated.";

// Private logger name and fixed session GUID for our SystemTraceProvider session.
// A non-admin process may start this session only if setup-pmu-access.ps1 has ACL'd the user's SID onto this exact GUID (EventAccessControl).
// The GUID here and the one in the script MUST stay identical.
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

bool iequals(cc::string const& a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < b.size(); ++i)
        if (to_lower(a[i]) != to_lower(b[i]))
            return false;
    return true;
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

// -- events the HAL lacks, registered for this boot ----------------------------------------------------------
//
// Windows' built-in AMD sources define DcacheMisses and CacheMisses with event 0 and no unit mask, so they count nothing.
// The kernel registers a raw event as a new profile source through NtSetSystemInformation(SystemPerformanceTraceInformation), an undocumented class.
// The layout below is what the kernel reads, checked against its handler in Windows 11 24H2; phnt's ntexapi.h names it EVENT_TRACE_PROFILE_ADD_INFORMATION.
// The caller must be in Administrators or Performance Monitor Users (S-1-5-32-558), which tools/setup-pmu-access.ps1 grants.
// Without that the registration is denied, and the counters it would have backed stay unavailable.
// A registration is not persisted, so it lasts until reboot; registering the same name with the same event again succeeds.

constexpr ULONG s_system_performance_trace_information = 31;
constexpr ULONG s_event_trace_profile_source_add_information = 21;

struct profile_source_add
{
    ULONG info_class;
    UCHAR version;    // 3: a 16-bit AMD event select
    struct alignas(4) // the kernel's union also holds a ULONG
    {
        USHORT event_select;
        UCHAR unit_select;
        UCHAR cmask;
        UCHAR cinv;
        UCHAR edge;
        UCHAR host_guest;
        UCHAR pmu_type;
    } amd;
    ULONG cpu_family_model_stepping[3]; // ~0 matches any
    ULONG initial_interval;
    BOOLEAN persist;
    WCHAR name[64];
};
static_assert(offsetof(profile_source_add, amd) == 0x08 && offsetof(profile_source_add, cpu_family_model_stepping) == 0x10
                  && offsetof(profile_source_add, initial_interval) == 0x1c
                  && offsetof(profile_source_add, persist) == 0x20 && offsetof(profile_source_add, name) == 0x22,
              "the layout the kernel reads");

// An event of one CPU family, named so that source_matches() maps it to its logical counter.
struct raw_event
{
    char const* name;
    USHORT event_select;
    UCHAR unit_select;
};

// AMD family 1Ah, Zen 5, from AMD's event list as Linux perf carries it (amdzen5/load-store.json).
// Calibrated against row- and column-major walks of 256 MiB: 224 M, 42 k and 2.5 k L1D misses for column-major, row-major and an L1-resident loop.
constexpr raw_event s_zen5_events[] = {
    {"Zen5L1DMisses", 0x41, 0x07}, // ls_mab_alloc.load_store_allocations: miss address buffers for demand loads and stores
    {"Zen5LLCReferences", 0x43, 0xfe}, // ls_dmnd_fills_from_sys, all but local L2: demand fills that reached L3 or beyond
    {"Zen5LLCMisses", 0x43, 0x48},     // ls_dmnd_fills_from_sys.dram_io_all: demand fills from DRAM or IO
};

// The events this CPU needs registered, or none.
cc::span<raw_event const> missing_events()
{
    auto const vendor = cc::impl::cpuid(0);
    auto const amd = vendor.ebx == 0x68747541 && vendor.edx == 0x69746e65 && vendor.ecx == 0x444d4163; // "AuthenticAMD"
    if (!amd)
        return {};
    auto const eax = cc::impl::cpuid(1).eax;
    auto const base_family = (eax >> 8) & 0xf;
    auto const family = base_family == 0xf ? base_family + ((eax >> 20) & 0xff) : base_family;
    if (family == 0x1a)
        return s_zen5_events;
    return {};
}

void register_missing_events()
{
    auto const events = missing_events();
    if (events.empty())
        return;
    using nt_set_system_information_t = LONG(NTAPI*)(ULONG, void*, ULONG);
    auto const nt_set = reinterpret_cast<nt_set_system_information_t>(
        ::GetProcAddress(::GetModuleHandleA("ntdll.dll"), "NtSetSystemInformation"));
    if (nt_set == nullptr)
        return;
    for (auto const& e : events)
    {
        auto info = profile_source_add{};
        info.info_class = s_event_trace_profile_source_add_information;
        info.version = 3;
        info.amd.event_select = e.event_select;
        info.amd.unit_select = e.unit_select;
        for (auto& c : info.cpu_family_model_stepping)
            c = ~ULONG(0);
        info.initial_interval = 0x10000;
        info.persist = FALSE;
        for (auto i = 0; e.name[i] != 0 && i < 63; ++i)
            info.name[i] = WCHAR(e.name[i]);
        (void)nt_set(s_system_performance_trace_information, &info,
                     ULONG(sizeof(info))); // denied without the group: the counter stays unavailable
    }
}

// Enumerate the CPU's PMU profile sources once (the list is stable for the process lifetime).
// The events the HAL lacks are registered first, so they enumerate with the rest.
cc::vector<profile_source> const& profile_sources()
{
    static auto const sources = []
    {
        register_missing_events();
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

// Does an ETW profile-source name look like the given logical counter?
// Names are CPU-specific, so match a few known spellings; the greedy assignment keeps one source from being claimed twice.
// A name that is a substring of another source's name is matched exactly: "CacheMisses" must not claim "IcacheMisses".
// Calibrated on a Zen 5 under Windows 11 against row- and column-major walks of 256 MiB:
// DcacheAccesses and DCAccess count L1D accesses, IcacheMisses and ICMiss instruction-cache misses, TotalCycles core cycles.
// DcacheMisses and CacheMisses read 0 there through any workload; silent_sources() is what keeps such a source unclaimed.
bool source_matches(hw_counter c, cc::string const& name)
{
    switch (c)
    {
    case hw_counter::instructions_retired:
        return icontains(name, "InstructionRetired") || icontains(name, "InstructionsRetired")
            || iequals(name, "TotalIssues");
    case hw_counter::branch_instructions:
        return icontains(name, "BranchInstruction");
    case hw_counter::branch_misses:
        return icontains(name, "BranchMispredict");
    case hw_counter::cache_l1d_misses:
        return icontains(name, "DcacheMiss") || icontains(name, "L1DMiss") || icontains(name, "L1DCacheMiss")
            || icontains(name, "DataCacheMiss");
    case hw_counter::cache_llc_references:
        return icontains(name, "LLCReference") || icontains(name, "LLCRef");
    case hw_counter::cache_llc_misses:
        return icontains(name, "LLCMiss") || iequals(name, "CacheMisses");
    case hw_counter::core_cycles:
        return iequals(name, "TotalCycles") || icontains(name, "UnhaltedCoreCycles");
    case hw_counter::cache_l1d_accesses:
        return icontains(name, "DcacheAccess") || iequals(name, "DCAccess");
    default:
        return false;
    }
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

// Fill a zeroed EVENT_TRACE_PROPERTIES for our private real-time system logger.
// `buffer` must be s_props_size bytes.
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

// Start our session, replacing any stale instance left by a crashed run.
// `props` must be s_props_size bytes.
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

// Attach the PMC counters to context-switch events; returns true when both calls succeed.
// Both TraceSetInformation calls are required.
// Without the event-list call the counters are configured but attached to no event, so every CSwitch arrives with no PMC extended-data item.
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

// Can this process actually read PMU counters right now?
// Probed once by really starting the session and stopping it again, which is the honest test of the privilege + ACL gate — StartTrace returns ERROR_ACCESS_DENIED without it.
// Deliberately does NOT bind PMC sources: that leaves configuration residue which makes the next real measure's TracePmcCounterListInfo fail.
// The answer is cached and stable for the process, so re-running setup-pmu-access.ps1 needs a fresh process to take effect here.
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
// The consumer is the only writer of totals / last_in / running.
// The measuring thread reads totals only after the consumer has stopped and been joined, so no locking is needed there.
// t_arm / t_disarm bound the counted window and are read live by the consumer, so they are atomic.
struct pmc_state
{
    // Logical processors we track per-CPU counter snapshots for.
    // An event on a higher-numbered CPU is ignored and that interval goes uncounted, which is only reachable on a >256-way machine.
    static constexpr int max_cpus = 256;

    u32 target_tid = 0;
    isize counter_count = 0;
    std::atomic<i64> t_arm = {0};
    std::atomic<i64> t_disarm = {INT64_MAX};

    bool running[max_cpus] = {};
    u64 last_in[max_cpus][MAX_HW_COUNTERS] = {};
    u64 totals[MAX_HW_COUNTERS] = {};

    /// How many per-interval deltas were folded into totals.
    /// Programming the counters and actually reading them are different things.
    /// A session can start and configure its sources while no context switch ever delivers PMC extended data, which is what a virtualized host does.
    /// Zero folds means the totals are untouched rather than measured, so the samples must come back invalid — otherwise a caller cannot tell "this counter read zero" from "this counter never ran".
    u64 folds = 0;
};

// ProcessTrace callback, one call per delivered event.
// Only context switches carrying PMC extended data matter, and only while our benchmark thread is the one being switched in or out.
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
        ++st->folds;
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

// The outcome of running a body with profile sources attached.
struct source_run
{
    bool live = false;    ///< the session ran with at least one source programmed
    isize configured = 0; ///< how many of the requested sources fit the PMC budget, from the first
    u64 folds = 0;        ///< scheduling intervals folded into totals; zero means nothing was read
    u64 totals[MAX_HW_COUNTERS] = {};
    f64 secs = 0;
    u64 cycles = 0;
};

// Runs `body` once with `count` profile sources attached to the context switches that bracket it, and reads the baseline tightly around it.
// Only a limited number of sources can be programmed at once, so `configured` may come back short of `count`.
source_run run_with_sources(cc::function_ref<void()> body, ULONG const* source_ids, isize count)
{
    auto run = source_run{};
    auto state = pmc_state{};
    state.target_tid = ::GetCurrentThreadId();

    auto trace_session = TRACEHANDLE(0);
    auto consumer_handle = TRACEHANDLE(0);
    auto consumer_thread = HANDLE(nullptr);
    alignas(8) unsigned char session_props[s_props_size] = {};

    if (count > 0)
    {
        enable_system_profile_privilege();

        // Configuring every source fails with ERROR_BUSY when the budget, a few per core minus what other ETW sessions hold, is short.
        // Degrade to as many as fit: drop the last source and retry, so a caller gets its highest-priority counters rather than nothing.
        // Each attempt needs a fresh session, because one whose PMC configuration was rejected will not then accept a smaller one.
        for (auto want = count; want >= 1; --want)
        {
            if (start_session(trace_session, session_props) == ERROR_SUCCESS
                && configure_session_pmc(trace_session, source_ids, want))
            {
                run.configured = want;
                break;
            }
            if (trace_session != 0)
            {
                ::ControlTraceA(trace_session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(session_props),
                                EVENT_TRACE_CONTROL_STOP);
                trace_session = 0;
            }
        }

        if (run.configured > 0)
        {
            state.counter_count = run.configured; // the consumer reads exactly this many PMC values per event

            auto logfile = EVENT_TRACE_LOGFILEA{};
            logfile.LoggerName = const_cast<char*>(s_session_name);
            logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
            logfile.EventRecordCallback = &on_event;
            logfile.Context = &state;

            consumer_handle = ::OpenTraceA(&logfile);
            if (consumer_handle != INVALID_PROCESSTRACE_HANDLE)
            {
                consumer_thread = ::CreateThread(nullptr, 0, &consumer_proc, &consumer_handle, 0, nullptr);
                run.live = consumer_thread != nullptr;
            }
        }
    }

    // Arm the counted window and bracket the single body() call with forced context switches.
    // The window clock must match the event timestamps (FILETIME).
    auto now_ts = [&]
    {
        auto ft = FILETIME{};
        ::GetSystemTimePreciseAsFileTime(&ft);
        return i64((u64(ft.dwHighDateTime) << 32) | u64(ft.dwLowDateTime));
    };

    if (run.live)
    {
        state.t_arm.store(now_ts(), std::memory_order_relaxed);
        force_context_switch(); // switch out + back in -> a switch-in event snapshots the start counters
    }

    auto const cycles_begin = cc::current_cycles();
    auto const secs_begin = cc::current_time_steady_secs();
    body();
    auto const secs_end = cc::current_time_steady_secs();
    auto const cycles_end = cc::current_cycles();

    if (run.live)
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

    run.secs = secs_end - secs_begin;
    run.cycles = cycles_end - cycles_begin;
    run.folds = state.folds;
    for (auto i = isize(0); i < run.configured; ++i)
        run.totals[i] = state.totals[i];
    return run;
}

// A workload every working counter sees: loads that miss to memory, branches on random data, and millions of cycles.
// `table` spans 128 MiB, past any last-level cache, with every page written once: loads from untouched pages all read one shared zero page and hit in cache.
void probe_workload(cc::span<u64 const> table)
{
    auto x = u64(0x9E37'79B9'7F4A'7C15ull);
    auto sum = u64(0);
    for (auto i = 0; i < (1 << 18); ++i)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        if (x & 1) // random, so mispredicted about half the time
            sum += table[isize(x % u64(table.size()))];
        else
            sum ^= x >> 3;
    }
    static auto volatile sink = u64(0);
    sink = sum;
}

// Profile sources that program and run but never count, as DcacheMisses and CacheMisses on a Zen 5 under Windows 11.
// Probed once per process: the source each logical counter would claim runs through probe_workload(), and one still at 0 is silent.
// A silent source hands its counter on to the next candidate, which is probed in turn.
// Every pass is a session of its own, about 75 ms, so passes take four sources: a budget every PMU seen so far meets, which no retry has to discover.
// A run that reads nothing at all decides nothing, so without readable PMCs no source is silent.
cc::vector<bool> const& silent_sources()
{
    static auto const silent = []
    {
        auto const& sources = profile_sources();
        auto out = cc::vector<bool>::create_defaulted(sources.size());
        if (!pmu_readable())
            return out;

        auto probed = cc::vector<bool>::create_defaulted(sources.size());
        auto table = cc::vector<u64>::create_uninitialized(isize(16) << 20);
        for (auto i = isize(0); i < table.size(); i += 4096 / isize(sizeof(u64)))
            table[i] = u64(i);

        while (true)
        {
            // each counter's candidate: its first matching source not known to be silent
            cc::vector<isize> batch;
            for (auto const c : s_pmu_counters)
                for (auto i = isize(0); i < sources.size(); ++i)
                    if (!out[i] && source_matches(c, sources[i].name))
                    {
                        auto queued = false;
                        for (auto const b : batch)
                            queued = queued || b == i;
                        if (!probed[i] && !queued)
                            batch.push_back(i);
                        break;
                    }
            if (batch.empty())
                return out;

            for (auto begin = isize(0); begin < batch.size();)
            {
                ULONG ids[4] = {};
                auto const count = cc::min(batch.size() - begin, isize(4));
                for (auto k = isize(0); k < count; ++k)
                    ids[k] = sources[batch[begin + k]].id;
                auto const run = run_with_sources([&] { probe_workload(table); }, ids, count);
                if (!run.live || run.configured == 0 || run.folds == 0)
                    return out;
                for (auto k = isize(0); k < run.configured; ++k)
                {
                    probed[batch[begin + k]] = true;
                    out[batch[begin + k]] = run.totals[k] == 0;
                }
                begin += run.configured;
            }
        }
    }();
    return silent;
}

// Index of the first not-yet-claimed, counting profile source matching `c`, or -1.
isize find_source(hw_counter c, cc::span<bool> used)
{
    auto const& sources = profile_sources();
    auto const& silent = silent_sources();
    for (auto i = isize(0); i < sources.size(); ++i)
        if (!used[i] && !silent[i] && source_matches(c, sources[i].name))
            return i;
    return -1;
}
} // namespace

cc::vector<backend_counter> backend_enumerate_counters()
{
    cc::vector<backend_counter> out;
    out.push_back({.id = hw_counter::elapsed_nanoseconds, .available = true});
    out.push_back({.id = hw_counter::reference_cycles, .available = cc::has_cycle_counter()});

    // A PMU counter is measurable only if a counting source exists AND we can actually start the session to read it.
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

    ULONG source_ids[MAX_HW_COUNTERS] = {};
    for (auto i = isize(0); i < mapped.size(); ++i)
        source_ids[i] = mapped[i].source;
    auto const run = run_with_sources(body, source_ids, mapped.size());

    // A request whose counters all lack a counting source programs nothing, which is not missing access.
    if (pmu_requested && !mapped.empty() && !run.live)
        warn_pmu_unavailable_once(s_setup_hint);

    cc::vector<hw_counter_sample> out;
    out.reserve(counters.size());
    for (auto const c : counters)
    {
        auto name = cc::string(logical_counter_name(c));
        if (c == hw_counter::elapsed_nanoseconds)
        {
            out.push_back({.id = c, .name = name, .value = u64(run.secs * 1e9), .valid = true});
            continue;
        }
        if (c == hw_counter::reference_cycles)
        {
            out.push_back({.id = c, .name = name, .value = run.cycles, .valid = cc::has_cycle_counter()});
            continue;
        }

        // A PMU counter: its value sits at the same index in run.totals as in our mapped list.
        // Only the first `configured` sources were actually programmed; the rest lost the PMC-counter budget race.
        auto value = u64(0);
        auto valid = false;
        for (auto i = isize(0); i < mapped.size(); ++i)
        {
            if (mapped[i].id != c)
                continue;
            name = mapped[i].name;
            // run.folds is part of the condition because run.live only says the session programmed them.
            // A host that delivers no PMC data leaves every total at 0, and reporting that as a reading is what makes a
            // zero indistinguishable from a counter that never flowed.
            if (run.live && i < run.configured && run.folds > 0)
            {
                value = run.totals[i];
                valid = true;
            }
            break;
        }
        out.push_back({.id = c, .name = name, .value = value, .valid = valid});
    }
    return out;
}
} // namespace nx::bench::impl
