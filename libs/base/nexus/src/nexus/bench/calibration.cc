#include "calibration.hh"

#include <clean-core/common/time.hh>
#include <nexus/bench/barriers.hh>

using namespace cc::primitive_defines;

namespace
{
// Ticks per second, from a fixed window against the steady clock.
//
// Two milliseconds is long enough that the steady clock's own resolution is noise and short enough that nobody notices
// paying it, and the window is walked rather than slept through so a scheduler that parks the thread cannot stretch it.
f64 measure_seconds_per_tick()
{
    auto const wall_start = cc::current_time_steady_secs();
    auto const ticks_start = cc::current_cycles();

    auto wall_end = wall_start;
    while (wall_end - wall_start < 0.002)
        wall_end = cc::current_time_steady_secs();

    auto const ticks_end = cc::current_cycles();

    auto const elapsed_wall = wall_end - wall_start;
    auto const elapsed_ticks = f64(ticks_end - ticks_start);
    if (elapsed_ticks <= 0 || elapsed_wall <= 0)
        return 0;

    return elapsed_wall / elapsed_ticks;
}

// The minimum of several runs rather than their mean: the floor is what an undisturbed iteration costs, and every
// disturbance can only push a reading up.
f64 measure_min_per_iteration(f64 seconds_per_tick, isize iterations, auto&& one_pass)
{
    auto best = f64(0);
    for (auto attempt = isize(0); attempt < 8; ++attempt)
    {
        auto const t0 = cc::current_cycles();
        one_pass(iterations);
        auto const t1 = cc::current_cycles();

        auto const secs = f64(t1 - t0) * seconds_per_tick / f64(iterations);
        if (attempt == 0 || secs < best)
            best = secs;
    }
    return best;
}
} // namespace

nx::bench::calibration const& nx::bench::calibrated()
{
    static calibration const c = []
    {
        auto result = calibration{};
        result.has_cheap_counter = cc::has_cycle_counter();
        result.seconds_per_tick = measure_seconds_per_tick();

        constexpr auto iterations = isize(1) << 16;

        // The loop the engine actually runs, with the smallest body that cannot be deleted.
        result.empty_iteration_secs = measure_min_per_iteration(result.seconds_per_tick, iterations,
                                                                [](isize n)
                                                                {
                                                                    for (auto i = isize(0); i < n; ++i)
                                                                        bench::sink(i);
                                                                });

        // One clock pair per "iteration" here, so the per-iteration figure IS the pair's cost.
        result.clock_pair_secs = measure_min_per_iteration(result.seconds_per_tick, isize(1) << 12,
                                                           [](isize n)
                                                           {
                                                               for (auto i = isize(0); i < n; ++i)
                                                               {
                                                                   auto const a = cc::current_cycles();
                                                                   auto const b = cc::current_cycles();
                                                                   bench::sink(b - a);
                                                               }
                                                           });

        return result;
    }();

    return c;
}
