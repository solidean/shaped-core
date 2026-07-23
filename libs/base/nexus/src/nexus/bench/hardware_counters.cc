#include "hardware_counters.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

#include <atomic>

namespace nx::bench
{
namespace
{
// The portable default set. The first two are the always-on baseline; the rest need PMU access.
constexpr hw_counter s_default_set[] = {
    hw_counter::elapsed_nanoseconds,  hw_counter::reference_cycles, hw_counter::instructions_retired,
    hw_counter::branch_instructions,  hw_counter::branch_misses,    hw_counter::cache_l1d_misses,
    hw_counter::cache_llc_references, hw_counter::cache_llc_misses,
};

// Our best-effort explanation of what each counter measures. Deliberately terse; hardware counters are.
cc::string_view description_of(hw_counter c)
{
    switch (c)
    {
    case hw_counter::elapsed_nanoseconds:
        return "wall-clock time of the run (steady clock)";
    case hw_counter::reference_cycles:
        return "reference cycles (x86 TSC / thread cycle time); tracks wall-time, not core clocks under scaling";
    case hw_counter::instructions_retired:
        return "instructions that actually retired (completed), excluding speculatively-executed ones";
    case hw_counter::branch_instructions:
        return "retired branch instructions (taken and not-taken)";
    case hw_counter::branch_misses:
        return "retired branches the predictor got wrong; each costs a pipeline flush (~15-20 cycles)";
    case hw_counter::cache_l1d_misses:
        return "L1 data-cache read misses (the load had to go to L2 or beyond)";
    case hw_counter::cache_llc_references:
        return "accesses that reached the last-level cache";
    case hw_counter::cache_llc_misses:
        return "last-level-cache misses; typically a main-memory access (~hundreds of cycles)";
    }
    return "";
}
} // namespace

cc::string_view impl::logical_counter_name(hw_counter c)
{
    switch (c)
    {
    case hw_counter::elapsed_nanoseconds:
        return "elapsed_ns";
    case hw_counter::reference_cycles:
        return "ref_cycles";
    case hw_counter::instructions_retired:
        return "instructions";
    case hw_counter::branch_instructions:
        return "branches";
    case hw_counter::branch_misses:
        return "branch_misses";
    case hw_counter::cache_l1d_misses:
        return "l1d_misses";
    case hw_counter::cache_llc_references:
        return "llc_refs";
    case hw_counter::cache_llc_misses:
        return "llc_misses";
    }
    return "unknown";
}

cc::optional<u64> hw_measurement::value_of(hw_counter c) const
{
    for (auto const& s : samples)
        if (s.id == c && s.valid)
            return s.value;
    return {};
}

cc::span<hw_counter const> default_hw_counter_set()
{
    return s_default_set;
}

cc::vector<hw_counter_info> available_hw_counters()
{
    auto const backend = impl::backend_enumerate_counters();

    cc::vector<hw_counter_info> out;
    out.reserve(backend.size());
    for (auto const& c : backend)
        out.push_back({.id = c.id,
                       .name = c.native_name.empty() ? cc::string(impl::logical_counter_name(c.id)) : c.native_name,
                       .description = cc::string(description_of(c.id)),
                       .available = c.available});
    return out;
}

void print_hw_counters()
{
    auto const counters = available_hw_counters();

    auto available_count = 0;
    for (auto const& c : counters)
        if (c.available)
            ++available_count;

    cc::println("hardware counters ({} of {} measurable on this machine):", available_count, counters.size());
    for (auto const& c : counters)
        cc::println("  {} {}: {}", c.available ? "[x]" : "[ ]", c.name, c.description);

    // When a PMU exists but is not readable, say what to do about it (the [ ] rows above are the "missing").
    auto const hint = impl::backend_setup_hint();
    if (!hint.empty())
        cc::println("  missing PMU access — {}", hint);
}

hw_measurement measure_hw_counters(cc::function_ref<void()> body, hw_measure_config const& config)
{
    auto const requested
        = config.counters.has_value() ? cc::span<hw_counter const>(config.counters.value()) : default_hw_counter_set();

    // One pass: whatever fits the hardware's PMC budget is measured; the rest come back invalid.
    auto best = impl::backend_measure(body, requested);
    if (!config.measure_all)
        return {.samples = best};

    auto is_baseline
        = [](hw_counter c) { return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles; };

    // The PMU counters still lacking a value after the passes so far, in request order.
    auto still_missing = [&]
    {
        cc::vector<hw_counter> out;
        for (auto const& s : best)
            if (!s.valid && !is_baseline(s.id))
                out.push_back(s.id);
        return out;
    };

    // measure_all: re-run the body over the not-yet-measured counters, each pass grabbing a budget-sized
    // chunk (the backend degrades from the end), until every requested counter has a value — or a pass adds
    // nothing, meaning no PMU counter is readable at all (then stop rather than loop forever).
    for (auto missing = still_missing(); !missing.empty(); missing = still_missing())
    {
        cc::vector<hw_counter> subset;
        subset.reserve(missing.size() + 2);
        subset.push_back(hw_counter::elapsed_nanoseconds); // baseline is free (no PMC slot) and re-armed anyway
        subset.push_back(hw_counter::reference_cycles);
        for (auto const c : missing)
            subset.push_back(c);

        auto const pass = impl::backend_measure(body, subset);
        auto progressed = false;
        for (auto const& s : pass)
        {
            if (!s.valid || is_baseline(s.id))
                continue;
            for (auto& b : best)
                if (b.id == s.id && !b.valid)
                {
                    b = s;
                    progressed = true;
                    break;
                }
        }
        if (!progressed)
            break;
    }

    return {.samples = best};
}
} // namespace nx::bench

void nx::bench::impl::warn_pmu_unavailable_once(cc::string_view platform_hint)
{
    static std::atomic<bool> already_warned{false};
    if (already_warned.exchange(true))
        return;

    cc::eprintln("nexus/bench: full hardware counters are unavailable; reporting elapsed time and cycles only.");
    cc::eprintln("  {}", platform_hint);
}
