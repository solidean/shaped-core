#include <clean-core/common/time.hh>
#include <clean-core/thread/spin.hh>
#include <nexus/test.hh>

// cc::current_time_steady_secs, cc::current_time_wall_secs and cc::current_cycles are the repo's timing seam,
// replacing <chrono> and the x86 intrinsic headers everywhere else.
// What is pinned here is the contract callers rely on, not the resolution, which is the platform's.

TEST("cc::current_time_steady_secs advances and never goes backwards")
{
    auto const t0 = cc::current_time_steady_secs();
    CHECK(t0 > 0);

    auto last = t0;
    for (auto i = 0; i < 64; ++i)
    {
        auto const now = cc::current_time_steady_secs();
        CHECK(now >= last); // steady: monotonic by definition, and this is the property everything else assumes
        last = now;
    }

    // Busy-wait rather than sleep: this must stay fast, and a sleep would measure the scheduler.
    while (cc::current_time_steady_secs() - t0 < 1e-4)
        cc::spin_pause();

    CHECK(cc::current_time_steady_secs() - t0 >= 1e-4);
}

TEST("cc::current_time_wall_secs reads the Unix epoch")
{
    auto const t0 = cc::current_time_wall_secs();

    // 2020-01-01 and 2100-01-01. Wide enough that a badly-set machine clock does not fail the suite, narrow enough
    // that the two things this could plausibly be wrong about — a different epoch, or the wrong unit — both fail.
    CHECK(t0 > 1577836800.0);
    CHECK(t0 < 4102444800.0);

    // Busy-wait on the STEADY clock, which is the only one that cannot stall or step here.
    auto const s0 = cc::current_time_steady_secs();
    while (cc::current_time_steady_secs() - s0 < 1e-3)
        cc::spin_pause();

    // Advances over a millisecond of real time, but deliberately not asserted MONOTONIC: the wall clock may step
    // backwards when the system clock is set, which is exactly why it is not the one to measure a duration with.
    CHECK(cc::current_time_wall_secs() > t0 - 1.0);
}

TEST("cc::current_cycles advances on every architecture")
{
    // No has_cycle_counter() branch any more: that predicate says whether the tick is CHEAP, not whether it works.
    // A tick is monotonic everywhere — the TSC on x86, CNTVCT_EL0 on ARM64, the steady clock in nanoseconds on WASM —
    // and a caller that only needs a timestamp should never have had to ask.
    auto const c0 = cc::current_cycles();
    CHECK(c0 != 0);

    // A counter is constant-rate but not instruction-ordered, so only a loop long enough to dwarf that is guaranteed
    // to show an increase — a single back-to-back pair is not.
    auto const t0 = cc::current_time_steady_secs();
    while (cc::current_time_steady_secs() - t0 < 1e-4)
        cc::spin_pause();

    CHECK(cc::current_cycles() > c0);
}
