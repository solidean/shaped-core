#include <nexus/bench/impl/baseline.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

// Windows backend.
//
// Ships now: the always-on baseline — elapsed time and, on x86, a reference-cycle count — which works with
// no privileges anywhere, including on the virtualized GitHub-hosted CI runners where the PMU is hidden.
//
// Pending (tracked spike): the full PMU counters (instructions/branches/caches) via the Thread Profiling
// API. The intended wiring, once prototyped:
//   1. Enumerate the machine's PMU profile sources with TraceQueryInformation(TraceProfileSourceListInfo)
//      — this is what backend_enumerate_counters() should report as the native, available counters.
//   2. Select the desired sources as hardware counters via TraceSetInformation(TracePmcCounterProfileInfo).
//   3. Around the single body() invocation: EnableThreadProfiling(GetCurrentThread(), 0, mask, &handle),
//      run, ReadThreadProfilingData(handle, ..., &data), DisableThreadProfiling(handle); read the per-slot
//      HwCounters[] deltas out of the returned PERFORMANCE_DATA.
// That path needs either an elevated process (discouraged) or a one-time permission grant; the setup script
// tools/setup-pmu-access.ps1 adds the account to "Performance Log Users" and grants SeSystemProfilePrivilege
// so it works unelevated. Until it lands, PMU counters degrade and warn_pmu_unavailable_once() points there.

namespace nx::bench::impl
{
namespace
{
// The PMU counters this backend cannot yet measure (everything except the baseline pair).
constexpr hw_counter s_pmu_counters[] = {
    hw_counter::instructions_retired, hw_counter::branch_instructions,  hw_counter::branch_misses,
    hw_counter::cache_l1d_misses,     hw_counter::cache_llc_references, hw_counter::cache_llc_misses,
};

bool is_baseline(hw_counter c)
{
    return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles;
}

// The recommended fix, shown once when a caller asks for a PMU counter we cannot read.
constexpr char const* s_setup_hint = "run tools/setup-pmu-access.ps1 to grant the standard PMU permissions "
                                     "(or run elevated); full hardware counters on Windows are not wired up yet.";
} // namespace

cc::vector<backend_counter> backend_enumerate_counters()
{
    cc::vector<backend_counter> out;
    out.push_back({.id = hw_counter::elapsed_nanoseconds, .available = true});
    out.push_back({.id = hw_counter::reference_cycles, .available = has_reference_cycles()});
    for (auto const c : s_pmu_counters)
        out.push_back({.id = c, .available = false});
    return out;
}

cc::vector<hw_counter_sample> backend_measure(cc::function_ref<void()> body, cc::span<hw_counter const> counters)
{
    auto pmu_requested = false;
    for (auto const c : counters)
        if (!is_baseline(c))
            pmu_requested = true;

    // Bracket the single invocation with the baseline as tightly as possible.
    auto const cycles_begin = read_reference_cycles();
    auto const ns_begin = steady_now_ns();
    body();
    auto const ns_end = steady_now_ns();
    auto const cycles_end = read_reference_cycles();

    if (pmu_requested)
        warn_pmu_unavailable_once(s_setup_hint);

    cc::vector<hw_counter_sample> out;
    out.reserve(counters.size());
    for (auto const c : counters)
    {
        auto const name = cc::string(logical_counter_name(c));
        switch (c)
        {
        case hw_counter::elapsed_nanoseconds:
            out.push_back({.id = c, .name = name, .value = ns_end - ns_begin, .valid = true});
            break;
        case hw_counter::reference_cycles:
            out.push_back({.id = c, .name = name, .value = cycles_end - cycles_begin, .valid = has_reference_cycles()});
            break;
        default:
            out.push_back({.id = c, .name = name, .value = 0, .valid = false});
            break;
        }
    }
    return out;
}
} // namespace nx::bench::impl
