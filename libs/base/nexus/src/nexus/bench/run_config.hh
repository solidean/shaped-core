#pragma once

#include <nexus/bench/fwd.hh>

// What a measured run is allowed to cost, and when it is allowed to stop.
//
// Every default here produces a defensible measurement, so `nx::bench::run("name", body)` is a complete benchmark and
// the config is what an author reaches for to say something the defaults got wrong.

/// The knobs of one measured run.
///
/// Written as a designated-initializer literal at the call site rather than filled field by field:
///
///     nx::bench::run("cc::sort", {.min_time_secs = 2.0}, body);
///
/// `standard()` and `single_shot()` are the two named starting points.
/// There deliberately are not more: everything else is one initializer away, and naming a preset for it invites
/// picking it by its name rather than by what the workload needs.
struct nx::bench::run_config
{
    /// The floor on measurement effort: sampling never stops before this much time has gone into it.
    f64 min_time_secs = 0.5;

    /// The ceiling.
    /// A run that has not converged by here stops anyway and says so.
    f64 max_time_secs = 5.0;

    /// The floor and ceiling on how many samples the statistics run over.
    /// Below about a dozen the interval on the median is too wide to be worth reporting, which is what the floor is for.
    ///
    /// **The ceiling has to be able to satisfy min_time_secs at the batch size**, or the run stops on the cap having
    /// never spent the time that was asked for.
    /// A batch takes about target_batch_secs, so the ceiling wants to be at least `min_time_secs / target_batch_secs` —
    /// 500 for the defaults, which is what 1024 leaves room for.
    isize min_samples = 16;
    isize max_samples = 1024;

    /// Stop early once the answer is this precise: the CI95 half-width as a fraction of the median.
    /// Only ever *shortens* a run — min_time_secs and min_samples still have to be satisfied first.
    f64 target_relative_error = 0.02;

    /// Iterations run, and thrown away, before anything is measured.
    /// Caches, branch predictors and the allocator all settle here rather than inside the first sample.
    f64 warmup_time_secs = 0.1;

    /// A fixed warmup count instead of a time budget; 0 derives it from warmup_time_secs.
    isize warmup_iterations = 0;

    /// Whether iterations are grouped into batches timed as one span.
    ///
    /// **On is what makes a nanosecond-scale body measurable at all**: one clock read costs more than the body, so the
    /// read is amortized over a batch whose size the harness calibrates.
    /// Off means one iteration per sample, which is what a workload too expensive to repeat wants.
    bool batch = true;

    /// How long one batch should take, which is what the calibrated batch size is chosen to hit.
    /// Far above the clock's own resolution and far above the harness's per-iteration cost, so neither shows up in the
    /// result.
    f64 target_batch_secs = 0.001;

    /// Emit a `compiler_barrier` after every iteration.
    ///
    /// Off by default: a harness that inhibits optimizations on the author's behalf measures something the author did
    /// not write.
    /// The `body_deleted` warning is what catches the failure this would have prevented.
    bool clobber_each_iteration = false;

    /// Whether the harness-overhead warning may fire at all.
    /// Off under single_shot, where harness cost against a one-second body is not something anyone needs telling about.
    bool warn_on_overhead = true;

    /// Measure hardware counters, in passes run after the timing has converged.
    ///
    /// **Never inside a timed region**, so an unavailable PMU costs the timings nothing and a present one does not
    /// change them either.
    /// One extra pass over the body, which is nothing next to a half-second of sampling and is the whole cost of a
    /// single_shot workload — which is why single_shot turns it off.
    bool measure_counters = true;

    /// Measure every requested counter, re-running the body once per subset that fits the hardware.
    ///
    /// Only a few PMU counters can be programmed at once, so a single pass silently drops the rest.
    /// Off by default: the multiplication is paid by every benchmark, and the default counter set mostly fits.
    bool multiplex_counters = false;

    /// This loop is what the others in the same benchmark are compared against.
    /// With no loop marked, the first one declared is the baseline; see the comparison report.
    bool is_baseline = false;

    /// The defaults above: an adaptive, batched run of about half a second.
    [[nodiscard]] static constexpr run_config standard() { return {}; }

    /// One iteration per sample, for a workload that is too expensive to repeat or that would not survive repetition.
    ///
    /// Turns off the two things the sampling model otherwise assumes: batching, and the batch-size calibration behind
    /// it.
    /// The overhead warning goes with them, since harness cost cannot be a meaningful share of a body this size.
    ///
    /// **It still warms up once.** A first iteration that is systematically slower than the rest corrupts the median
    /// rather than showing up as an honest outlier, and paying for one extra run is the cheaper mistake.
    /// A deliberately cold measurement is `evict_data_caches` inside the body, not a missing warmup.
    [[nodiscard]] static constexpr run_config single_shot()
    {
        auto c = run_config{};
        c.batch = false;
        c.warmup_iterations = 1;
        c.warmup_time_secs = 0;
        c.min_time_secs = 0;
        c.max_time_secs = 60;
        c.min_samples = 8;
        c.warn_on_overhead = false;
        // One counter pass over a body this expensive is another whole run of it, which is the thing single_shot
        // exists to avoid.
        c.measure_counters = false;
        return c;
    }
};
