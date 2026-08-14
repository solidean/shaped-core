#include <clean-core/common/time.hh>
#include <clean-core/thread/spin.hh>
#include <nexus/test.hh>

// cc::current_time_steady_secs and cc::current_cycles are the repo's timing seam, replacing <chrono> and
// the x86 intrinsic headers everywhere else.
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

TEST("cc::current_cycles advances where the architecture has a counter")
{
    if constexpr (cc::has_cycle_counter())
    {
        auto const c0 = cc::current_cycles();
        CHECK(c0 != 0);

        // The TSC is constant-rate but not instruction-ordered, so only a loop long enough to dwarf that
        // is guaranteed to show an increase — a single back-to-back pair is not.
        auto const t0 = cc::current_time_steady_secs();
        while (cc::current_time_steady_secs() - t0 < 1e-4)
            cc::spin_pause();

        CHECK(cc::current_cycles() > c0);
    }
    else
    {
        // Documented fallback: 0, so a caller can branch on has_cycle_counter() rather than on a magic value.
        CHECK(cc::current_cycles() == 0);
    }
}
