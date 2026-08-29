#include "hardware_counters.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

#include <atomic>

namespace nx::bench
{
namespace
{
// The portable default set.
// The first two are the always-on baseline; the rest need PMU access.
constexpr hw_counter s_default_set[] = {
    hw_counter::elapsed_nanoseconds,  hw_counter::reference_cycles, hw_counter::instructions_retired,
    hw_counter::branch_instructions,  hw_counter::branch_misses,    hw_counter::cache_l1d_misses,
    hw_counter::cache_llc_references, hw_counter::cache_llc_misses,
};

// Our best-effort explanation of what each counter measures, deliberately terse.
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

    // Counters a pass of their own could not read: unreadable on this machine rather than casualties of the budget.
    cc::vector<hw_counter> retired;
    auto is_retired = [&](hw_counter c)
    {
        for (auto const r : retired)
            if (r == c)
                return true;
        return false;
    };

    // The PMU counters still lacking a value and still worth another pass, in request order.
    auto still_missing = [&]
    {
        cc::vector<hw_counter> out;
        for (auto const& s : best)
            if (!s.valid && !is_baseline(s.id) && !is_retired(s.id))
                out.push_back(s.id);
        return out;
    };

    // measure_all: re-run the body over the not-yet-measured counters until each one has a value or has been shown unreadable.
    // The budget must never be what ends this loop — only a counter this machine cannot deliver may be given up on.
    //
    // The simultaneous-counter budget cannot be asked for up front, and is not the PMU's nominal counter count: whatever else already holds a PMC (the NMI watchdog, typically) shrinks it.
    // A group that overshoots it is refused when the values are read rather than when the events are opened, so an over-wide pass looks exactly like a pass with no PMU access at all.
    // Narrowing is what tells the two apart: halve the chunk and retry.
    // The width that works is then kept for the following passes, instead of re-widening into the same refusal.
    //
    // A counter still invalid after a pass carrying it alone is unreadable on its own terms, so it is retired and the ones behind it carry on.
    // Breaking out there instead would let one unsupported event decide that everything after it goes unmeasured.
    auto budget = isize(0);
    for (auto missing = still_missing(); !missing.empty(); missing = still_missing())
    {
        if (budget == 0)
            budget = missing.size();
        auto const take = cc::min(budget, missing.size());

        cc::vector<hw_counter> subset;
        subset.reserve(take + 2);
        subset.push_back(hw_counter::elapsed_nanoseconds); // baseline is free (no PMC slot) and re-armed anyway
        subset.push_back(hw_counter::reference_cycles);
        for (auto i = isize(0); i < take; ++i)
            subset.push_back(missing[i]);

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
        {
            if (take <= 1)
                retired.push_back(missing[0]);
            else
                budget = (take + 1) / 2;
        }
    }

    return {.samples = best};
}
} // namespace nx::bench

void nx::bench::impl::warn_pmu_unavailable_once(cc::string_view platform_hint)
{
    static std::atomic<bool> already_warned = {false};
    if (already_warned.exchange(true))
        return;

    cc::eprintln("nexus/bench: full hardware counters are unavailable; reporting elapsed time and cycles only.");
    cc::eprintln("  {}", platform_hint);
}
