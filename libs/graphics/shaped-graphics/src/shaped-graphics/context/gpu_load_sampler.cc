#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/gpu_metrics.hh>

namespace sg
{
namespace
{
/// A busy fraction from two readings of one engine's counter, clamped into [0, 1].
///
/// The clamp is not cosmetic: the counters are sampled per engine at slightly different instants, and a driver may
/// reset one, either of which produces a ratio outside the range that a caller would draw as a bar running off the end.
f32 engine_busy(f64 before, f64 after, f64 interval)
{
    if (interval <= 0 || after < before)
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
