#include "run.hh"

#include <clean-core/common/time.hh>
#include <clean-core/record/desc.hh>
#include <clean-core/string/format.hh>
#include <nexus/bench/calibration.hh>
#include <nexus/bench/statistics.hh>
#include <nexus/tests/execute.hh>

using namespace cc::primitive_defines;

namespace
{
// Above this share of per-iteration time, the harness is a meaningful part of what was measured.
constexpr auto overhead_warn_fraction = f64(0.02);

// Below this share of the empty-loop floor, the body cannot have run: it was optimized away.
constexpr auto deleted_body_fraction = f64(0.5);

// Above this share of the measured span spent paused, the pause pairs are most of the measurement.
constexpr auto paused_warn_fraction = f64(0.25);

// A batch may not grow past this however cheap the body is, so a bad per-iteration estimate cannot produce a batch
// that runs for minutes before the first sample arrives.
constexpr auto max_batch_size = isize(1) << 24;
} // namespace

struct nx::bench::impl::run_state
{
    calibration const* cal = nullptr;

    bool is_warmup = false;

    // Cleared per batch.
    u64 paused_ticks = 0;
    u64 pause_started_at = 0;
    bool is_paused = false;

    // Accumulated over MEASURED iterations only; warmup contributes nothing.
    isize items = 0;

    struct quantity_acc
    {
        cc::string name;
        cc::rec::unit const* unit = nullptr;

        // Sum and count are enough for both combination rules: a summing unit reports the sum, an averaging one
        // reports sum/count, which is the batch means weighted by iteration count.
        f64 sum = 0;
        isize count = 0;
    };
    cc::vector<quantity_acc> quantities;

    void begin_batch()
    {
        paused_ticks = 0;
        is_paused = false;
    }

    quantity_acc& quantity_for(cc::string_view name, cc::rec::unit const* unit)
    {
        for (auto& q : quantities)
            if (q.name == name)
                return q;

        quantities.push_back({.name = cc::string(name), .unit = unit});
        return quantities.back();
    }
};

void nx::bench::impl::advance(iteration& it, isize index)
{
    it._index = index;
}

void nx::bench::impl::bind(iteration& it, run_state* state)
{
    it._state = state;
}

void nx::bench::impl::begin_batch(iteration& it, bool warmup)
{
    it._items = 0;
    it._warmup = warmup;
}

isize nx::bench::impl::take_items(iteration& it)
{
    auto const n = it._items;
    it._items = 0;
    return n;
}

void nx::bench::iteration::pause()
{
    if (_state == nullptr || _state->is_paused)
        return;
    _state->is_paused = true;
    _state->pause_started_at = cc::current_cycles();
}

void nx::bench::iteration::resume()
{
    if (_state == nullptr || !_state->is_paused)
        return;
    _state->paused_ticks += cc::current_cycles() - _state->pause_started_at;
    _state->is_paused = false;
}

void nx::bench::iteration::record(cc::string_view name, cc::rec::unit const& unit, f64 value)
{
    if (_state == nullptr || _warmup)
        return;

    auto& q = _state->quantity_for(name, &unit);
    q.sum += value;
    ++q.count;
}

nx::bench::result nx::bench::impl::run_measured(cc::string_view name,
                                                run_config const& cfg,
                                                bool body_owns_loop,
                                                cc::function_ref<void(isize count, iteration& it)> body)
{
    auto const& cal = bench::calibrated();

    auto state = run_state{.cal = &cal};
    auto it = iteration{};
    impl::bind(it, &state);

    auto r = result{};
    r.name = cc::string(name);
    r.config = cfg;

    // Seconds for one batch of `count`, with any paused span already taken out.
    auto const time_batch = [&](isize count)
    {
        state.begin_batch();
        impl::begin_batch(it, state.is_warmup);

        auto const t0 = cc::current_cycles();
        body(count, it);
        auto const t1 = cc::current_cycles();

        // Collected once per batch rather than per iteration, which is what lets iteration::items inline to one add.
        auto const declared = impl::take_items(it);
        if (!state.is_warmup)
            state.items += declared;

        auto const gross = t1 - t0;
        auto const net = gross > state.paused_ticks ? gross - state.paused_ticks : u64(0);
        return f64(net) * cal.seconds_per_tick;
    };

    // ---------------------------------------------------------------------------------------------------------
    // Warmup.
    // Nothing here is measured; it exists so caches, predictors and the allocator settle before sample one.
    // ---------------------------------------------------------------------------------------------------------
    state.is_warmup = true;
    auto per_iteration_estimate = f64(0);

    if (cfg.warmup_iterations > 0)
    {
        auto const secs = time_batch(cfg.warmup_iterations);
        r.warmup_iterations = cfg.warmup_iterations;
        per_iteration_estimate = secs / f64(cfg.warmup_iterations);
    }
    else if (cfg.warmup_time_secs > 0)
    {
        // Doubling rather than a fixed count: the body's cost is unknown here, and a fixed count is either far too few
        // for a nanosecond body or far too many for a millisecond one.
        auto elapsed = f64(0);
        auto count = isize(1);
        while (elapsed < cfg.warmup_time_secs && r.warmup_iterations < max_batch_size)
        {
            auto const secs = time_batch(count);
            elapsed += secs;
            r.warmup_iterations += count;
            per_iteration_estimate = secs / f64(count);
            count = cc::min(count * 2, max_batch_size);
        }
    }
    else
    {
        // No warmup was asked for, but the batch size still needs an estimate to be chosen from.
        auto const probe = isize(64);
        per_iteration_estimate = time_batch(probe) / f64(probe);
    }

    state.is_warmup = false;

    // ---------------------------------------------------------------------------------------------------------
    // Batch size, so that one timing boundary covers enough work to be worth reading a clock for.
    // ---------------------------------------------------------------------------------------------------------
    r.batch_size = 1;
    if (cfg.batch && per_iteration_estimate > 0)
    {
        auto const wanted = cfg.target_batch_secs / per_iteration_estimate;
        r.batch_size = wanted <= 1 ? isize(1) : cc::min(isize(wanted) + 1, max_batch_size);
    }

    // ---------------------------------------------------------------------------------------------------------
    // Sampling.
    // ---------------------------------------------------------------------------------------------------------
    state.items = 0;
    state.quantities.clear();

    auto elapsed = f64(0);
    auto total_paused = f64(0);

    while (true)
    {
        auto const secs = time_batch(r.batch_size);
        total_paused += f64(state.paused_ticks) * cal.seconds_per_tick;

        r.samples.push_back(secs / f64(r.batch_size));
        r.measured_iterations += r.batch_size;
        elapsed += secs;

        auto const samples = isize(r.samples.size());

        auto const capped = samples >= cfg.max_samples || elapsed >= cfg.max_time_secs;
        auto const effort_met = samples >= cfg.min_samples && elapsed >= cfg.min_time_secs;

        // Only ever evaluated where the loop could actually stop, since a full sort per sample would otherwise be the
        // most expensive thing in the run.
        if (effort_met || capped)
        {
            auto const s = bench::compute_statistics(r.samples);
            auto const precise = !s.ci_is_bound && s.relative_error() <= cfg.target_relative_error;

            // Convergence is a statement about the ANSWER, not about how the loop ended.
            // A run that hit a cap having already reached the target precision has converged, and reporting otherwise
            // would turn a config whose caps cannot satisfy min_time_secs into a permanent false alarm.
            if (precise && effort_met)
            {
                r.converged = true;
                break;
            }
            if (capped)
            {
                r.converged = precise;
                break;
            }
        }
    }

    r.measured_seconds = elapsed;
    r.time = bench::compute_statistics(r.samples);
    r.paused_fraction = elapsed + total_paused > 0 ? total_paused / (elapsed + total_paused) : 0;

    // ---------------------------------------------------------------------------------------------------------
    // Derived figures.
    // ---------------------------------------------------------------------------------------------------------
    r.items = state.items;
    if (r.items > 0 && elapsed > 0)
        r.items_per_second = f64(r.items) / elapsed;

    for (auto const& q : state.quantities)
    {
        auto const averaging = q.unit != nullptr && q.unit->aggregate == cc::rec::aggregation::mean;

        auto out = recorded_quantity{.name = q.name, .unit = q.unit};
        out.total = averaging ? (q.count > 0 ? q.sum / f64(q.count) : 0) : q.sum;
        out.per_iteration = r.measured_iterations > 0 ? q.sum / f64(r.measured_iterations) : 0;
        // A rate over an averaged quantity is meaningless — a mean of ratios per second says nothing.
        out.per_second = averaging || elapsed <= 0 ? 0 : q.sum / elapsed;

        r.quantities.push_back(cc::move(out));
    }

    // Zero for the void(isize) form: the body owns its loop, so nothing of the harness sits between iterations.
    if (!body_owns_loop && cal.empty_iteration_secs > 0 && r.time.median > 0)
        r.overhead_fraction = cal.empty_iteration_secs / r.time.median;

    // ---------------------------------------------------------------------------------------------------------
    // What the harness noticed, which a reader would otherwise have to infer from the numbers.
    // ---------------------------------------------------------------------------------------------------------
    if (cal.empty_iteration_secs > 0 && r.time.median > 0
        && r.time.median < cal.empty_iteration_secs * deleted_body_fraction)
    {
        r.warnings.push_back({
            .kind = warning_kind::body_deleted,
            .severity = warning_severity::error,
            .detail = cc::format("{:.2f} ns per iteration is below the empty-loop floor of {:.2f} ns, so the body was "
                                 "optimized away — pass its result to nx::bench::sink",
                                 r.time.median * 1e9, cal.empty_iteration_secs * 1e9),
        });
    }
    else if (!body_owns_loop && cfg.warn_on_overhead && r.overhead_fraction > overhead_warn_fraction)
    {
        r.warnings.push_back({
            .kind = warning_kind::overhead_significant,
            .severity = warning_severity::warning,
            .detail = cc::format("the harness is about {:.1f}% of the measured per-iteration time — give the body an "
                                 "inner loop and take the void(isize) form, which has no per-iteration cost at all",
                                 r.overhead_fraction * 100),
        });
    }

    if (!r.converged)
    {
        r.warnings.push_back({
            .kind = warning_kind::did_not_converge,
            .severity = warning_severity::warning,
            .detail = cc::format(
                "stopped at {} samples over {:.2f} s with a relative error of {:.1f}%, short of the {:.1f}% asked for",
                r.samples.size(), elapsed, r.time.relative_error() * 100, cfg.target_relative_error * 100),
        });
    }

    if (r.paused_fraction > paused_warn_fraction)
    {
        r.warnings.push_back({
            .kind = warning_kind::paused_fraction_high,
            .severity = warning_severity::warning,
            .detail = cc::format("{:.0f}% of the span was spent paused, so the pause pairs are a large part of what "
                                 "was "
                                 "measured — move the setup out of the loop, or take the void(isize) form",
                                 r.paused_fraction * 100),
        });
    }

    if (r.time.ci_is_bound)
    {
        r.warnings.push_back({
            .kind = warning_kind::too_few_samples,
            .severity = warning_severity::note,
            .detail = cc::format("{} samples support no interval on the median, so the reported one is the sample "
                                 "range",
                                 r.samples.size()),
        });
    }

    // Hand a copy to the running test, so a BENCHMARK's loops are collected and reported without the body doing
    // anything about it.
    // A no-op outside a test, which is what keeps `run` usable from ordinary code and from an application.
    nx::impl::record_benchmark_result(r);

    return r;
}
