#include "run_async.hh"

#include <clean-core/common/time.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/calibration.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/statistics.hh>

using namespace cc::primitive_defines;

cc::shared_async<nx::bench::result> nx::bench::impl::run_awaited(cc::string_view name_view,
                                                                 run_config cfg,
                                                                 cc::unique_function<cc::shared_async<cc::unit>()> iteration)
{
    // Copied before the first suspend: the caller's view is not guaranteed to outlive it.
    auto const name = cc::string(name_view);
    auto const& cal = bench::calibrated();

    auto r = result{};
    r.name = name;
    r.config = cfg;
    r.batch_size = 1;

    // Warmup: nothing measured, and at least one iteration so a first-run cost never lands in sample one.
    auto warmup_elapsed = f64(0);
    while (r.warmup_iterations < 1
           || (cfg.warmup_iterations > 0 ? r.warmup_iterations < cfg.warmup_iterations
                                         : warmup_elapsed < cfg.warmup_time_secs))
    {
        auto const t0 = cc::current_cycles();
        co_await iteration();
        warmup_elapsed += f64(cc::current_cycles() - t0) * cal.seconds_per_tick;
        ++r.warmup_iterations;
    }

    // Sampling: one awaited iteration per sample, so measured and wall time are the same thing here.
    auto elapsed = f64(0);
    while (true)
    {
        auto const t0 = cc::current_cycles();
        co_await iteration();
        auto const secs = f64(cc::current_cycles() - t0) * cal.seconds_per_tick;

        r.samples.push_back(secs);
        ++r.measured_iterations;
        elapsed += secs;

        if (impl::sampling_should_stop(r, cfg, elapsed, elapsed))
            break;
    }

    r.measured_seconds = elapsed;
    r.time = bench::compute_statistics(r.samples);
    impl::finish_sampled_result(r, cfg, elapsed);
    co_return r;
}
