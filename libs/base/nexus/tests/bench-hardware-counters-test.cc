#include <clean-core/common/macros.hh>
#include <nexus/bench/bench.hh>
#include <nexus/test.hh>

// Verifies the nexus/bench hardware-counter API end to end.
//
// It must pass on the virtualized GitHub-hosted CI runners (where the PMU is hidden — only the baseline is
// produced) AND genuinely check real values on hardware. So: the baseline (elapsed time, and reference
// cycles on x86) is always asserted; the full PMU counters are asserted only when the machine reports them
// available, and otherwise noted as skipped.

namespace
{
// A sink the workload writes through so the optimizer cannot delete the measured loop.
cc::u64 volatile s_bench_sink = 0;

// A small, data-dependent workload with branches and memory traffic — enough to move every counter.
void run_workload(cc::u64 iterations)
{
    auto state = cc::u64(0x1234'5678'9abc'def0);
    for (auto i = cc::u64(0); i < iterations; ++i)
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        if ((state & 0x3f) == 0) // a hard-to-predict branch
            state ^= i;
    }
    s_bench_sink = state;
}
} // namespace

TEST("nexus bench - hardware counters query and measure")
{
    using nx::bench::hw_counter;

    // --- query ---
    auto const available = nx::bench::available_hw_counters();
    REQUIRE(!available.empty()); // the baseline is always reported

    auto is_available = [&](hw_counter c)
    {
        for (auto const& info : available)
            if (info.id == c)
                return info.available;
        return false;
    };

    // The baseline must always be measurable.
    CHECK(is_available(hw_counter::elapsed_nanoseconds));

    nx::bench::print_hw_counters(); // smoke: must not crash

    // --- measure (body invoked exactly once) ---
    auto const m = nx::bench::measure_hw_counters([] { run_workload(2'000'000); });

    // Elapsed time is always valid and non-zero for a workload this size.
    auto const elapsed = m.value_of(hw_counter::elapsed_nanoseconds);
    REQUIRE(elapsed.has_value());
    CHECK(elapsed.value() > 0);

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    // On x86 the rdtsc reference-cycle baseline is always available and must advance.
    auto const cycles = m.value_of(hw_counter::reference_cycles);
    REQUIRE(cycles.has_value());
    CHECK(cycles.value() > 0);
#endif

    // Full PMU counters: assert plausibility when the counter actually flowed, else note the skip. Gating on
    // the measured value (not on enumeration availability) keeps this honest everywhere: a real value is
    // checked on Linux/Windows machines that can read the PMU, and machines where it cannot flow — virtualized
    // CI, or a box without the one-time PMU setup — take the skip instead of failing.
    auto const instructions = m.value_of(hw_counter::instructions_retired);
    if (instructions.has_value())
    {
        CHECK(instructions.value() > 0);
        // A 2M-iteration loop retires well over a million instructions; a very loose sanity floor.
        CHECK(instructions.value() >= 1'000'000);
    }
    else
    {
        SUCCEED("PMU counters did not flow on this machine (baseline-only run) — expected on virtualized CI or "
                "without the one-time PMU setup");
    }
}

TEST("nexus bench - default counter set is non-empty and starts with the baseline")
{
    using nx::bench::hw_counter;

    auto const set = nx::bench::default_hw_counter_set();
    REQUIRE(set.size() >= 2);
    CHECK(set[0] == hw_counter::elapsed_nanoseconds);
    CHECK(set[1] == hw_counter::reference_cycles);
}

// Manual: print the hardware counters this machine can measure right now (and, when a PMU is present but not
// readable, the setup hint). Driven by `dev.py profiling counters`; run directly with the exact test name.
TEST("nexus bench - list hardware counters", nx::config::manual)
{
    nx::bench::print_hw_counters();
}

TEST("nexus bench - an explicit counter set is honored and the body runs once")
{
    using nx::bench::hw_counter;

    auto call_count = 0;
    auto const m = nx::bench::measure_hw_counters(
        [&] { ++call_count; },
        {.counters = cc::vector<hw_counter>{hw_counter::elapsed_nanoseconds, hw_counter::instructions_retired}});

    CHECK(call_count == 1);       // one pass -> invoked exactly once
    CHECK(m.samples.size() == 2); // one sample per requested counter
    CHECK(m.samples[0].id == hw_counter::elapsed_nanoseconds);
    CHECK(m.samples[1].id == hw_counter::instructions_retired);
}

TEST("nexus bench - measure_all gathers every available counter across passes")
{
    using nx::bench::hw_counter;

    // The default set has more PMU counters than fit the hardware budget at once. With measure_all the body
    // is re-run over subsets so every counter the machine can read comes back valid — none left behind.
    auto call_count = 0;
    auto const m = nx::bench::measure_hw_counters(
        [&]
        {
            ++call_count;
            run_workload(200'000);
        },
        {.measure_all = true});

    CHECK(call_count >= 1); // at least one pass (more when counters need several)
    for (auto const& info : nx::bench::available_hw_counters())
        if (info.available)
            CHECK(m.value_of(info.id).has_value()); // every measurable counter got a value
}
