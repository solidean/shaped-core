#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/gpu_metrics.hh>

namespace sg
{
namespace
{
/// A busy fraction from two readings of one engine's counter, capped at 1.
///
/// The cap is not cosmetic: the engines are sampled at slightly different instants, so a ratio can land just over the
/// range a caller would draw as a bar.
/// A counter that went BACKWARDS is not capped here — `sample` reports that as an error, since it is a reading this
/// cannot difference rather than one at the edge of its range.
f32 engine_busy(f64 before, f64 after, f64 interval)
{
    if (interval <= 0)
        return 0;

    auto const busy = f32((after - before) / interval);
    return busy > 1 ? 1.0f : busy;
}
} // namespace
} // namespace sg

bool sg::gpu_load_sampler::is_supported(sg::context const& ctx)
{
    return ctx.read_gpu_counters().has_value();
}

sg::gpu_load_sampler::gpu_load_sampler(sg::context const& ctx) : _ctx(&ctx)
{
    if (auto baseline = ctx.read_gpu_counters(); baseline.has_value())
    {
        _previous = cc::move(baseline.value());
        _previous_time_secs = cc::current_time_steady_secs();
        _has_baseline = true;
    }
}

cc::result<sg::gpu_load> sg::gpu_load_sampler::sample()
{
    auto current = _ctx->read_gpu_counters();
    if (current.has_error())
        return cc::error(cc::move(current.error()));

    auto const now = cc::current_time_steady_secs();

    if (!_has_baseline)
    {
        _previous = cc::move(current.value());
        _previous_time_secs = now;
        _has_baseline = true;
        return cc::error("no baseline yet; this call took one");
    }

    auto const interval = now - _previous_time_secs;
    auto& next = current.value();

    _per_engine.clear();
    auto busiest = 0.0f;

    // Matched by engine name rather than by position: the platform may report a different set of engines between two
    // readings, and pairing by index would then difference two unrelated counters.
    for (auto const& engine : next.engines)
    {
        auto before = 0.0;
        auto found = false;
        for (auto const& previous : _previous.engines)
            if (cc::string_view(previous.engine) == cc::string_view(engine.engine))
            {
                before = previous.busy_secs;
                found = true;
                break;
            }

        if (!found)
            continue; // an engine that appeared since the baseline has nothing to difference against yet

        // The counter is a sum over the processes currently using the engine, so one exiting takes its share of the
        // total with it and the sum falls.
        // That is a reading with no utilization in it rather than an idle GPU, so it is an error instead of a zero —
        // and since the next sample re-baselines against this one, it is a single lost interval.
        if (engine.busy_secs < before)
        {
            // Built before the move, which is what `engine` points into.
            auto message = cc::format("the {} GPU engine counter went backwards, from {} s to {} s", engine.engine,
                                      before, engine.busy_secs);

            // The baseline still advances: leaving the old one in place would fail every later sample against it too.
            _per_engine.clear();
            _previous = cc::move(next);
            _previous_time_secs = now;
            return cc::error(cc::move(message));
        }

        auto const busy = sg::engine_busy(before, engine.busy_secs, interval);
        _per_engine.push_back({.engine = engine.engine, .busy = busy});
        busiest = busy > busiest ? busy : busiest;
    }

    auto out = sg::gpu_load();
    out.interval_secs = interval;
    out.total = busiest;
    out.per_engine = _per_engine;

    _previous = cc::move(next);
    _previous_time_secs = now;

    return out;
}
