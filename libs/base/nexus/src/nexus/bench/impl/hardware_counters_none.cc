#include <nexus/bench/impl/baseline.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

// Fallback backend: no PMU. Used on platforms without a hardware-counter implementation (currently macOS,
// and any OS that is neither Windows nor Linux). Produces only the baseline — elapsed time and, on x86, a
// reference-cycle count — and reports every PMU counter as unavailable.

namespace nx::bench::impl
{
namespace
{
// The PMU counters this backend cannot measure (everything except the baseline pair).
constexpr hw_counter s_pmu_counters[] = {
    hw_counter::instructions_retired, hw_counter::branch_instructions,  hw_counter::branch_misses,
    hw_counter::cache_l1d_misses,     hw_counter::cache_llc_references, hw_counter::cache_llc_misses,
};

bool is_baseline(hw_counter c)
{
    return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles;
}
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

cc::string backend_setup_hint()
{
    // No PMU on this platform, so there is no setup that would make one readable — say nothing.
    return {};
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
        warn_pmu_unavailable_once("this platform has no hardware-counter backend yet (macOS/ARM64 is unsupported for "
                                  "now).");

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
