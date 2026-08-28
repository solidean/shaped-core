#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/fwd.hh>
#include <nexus/bench/run_config.hh>

namespace cc::rec
{
struct unit;
} // namespace cc::rec

/// What a sample vector says, beyond its mean.
///
/// A timing is a draw from a distribution with a hard floor, a long right tail, and occasional excursions when the
/// scheduler or the frequency governor interferes.
/// Reporting one number from that is how a benchmark lies confidently, so every field here exists to make a different
/// kind of dishonesty visible.
struct nx::bench::statistics
{
    isize sample_count = 0;

    /// The headline.
    /// Robust to the tail, unlike the mean, and it has an interval that means something, unlike the minimum.
    f64 median = 0;

    /// The least-disturbed observation, and the most optimistic one.
    /// Worth reporting and wrong to headline: no interval can be put on it, so "is this reliable" becomes unanswerable.
    f64 min = 0;
    f64 max = 0;

    f64 mean = 0;

    /// The mean with the top and bottom 10% of samples dropped.
    /// Here because it is what most cross-language benchmark tooling compares against.
    f64 trimmed_mean = 0;

    /// Median absolute deviation — the spread measure consistent with reporting a median.
    ///
    /// The only spread measure here, and standard deviation is deliberately absent: it needs a square root, which at
    /// this tier means either `<cmath>` (denied repo-wide) or a typed-geometry dependency taxing every test binary.
    /// The full sample vector travels in the result, so anyone who wants stddev can take it from there.
    f64 mad = 0;

    /// A 95% confidence interval on the MEDIAN, from the order statistics rather than from a bootstrap.
    ///
    /// The interval is `[samples[k], samples[n-1-k]]` for the largest k whose two binomial tails are each under 2.5%.
    /// That is exact for the median of iid samples, needs no resampling and no seed, and costs a sort the statistics
    /// were paying for anyway — where a bootstrap would be approximate, seeded, and a thousand times more work for a
    /// wider answer.
    ///
    /// **Degenerate below about six samples**, where no k satisfies the tails: the interval is then the full
    /// [min, max] and `ci_is_bound` says so, rather than a narrow interval nothing supports.
    f64 ci95_low = 0;
    f64 ci95_high = 0;
    bool ci_is_bound = false;

    /// Samples outside the Tukey fences, `[q1 - 1.5*iqr, q3 + 1.5*iqr]`.
    ///
    /// **Counted, never dropped.**
    /// A benchmark whose samples split into two modes has learned something — usually a frequency change or a cache
    /// effect — and trimming it away silently is exactly how a harness reports a clean number for a dirty run.
    isize outliers = 0;

    /// The CI95 half-width as a fraction of the median, which is what the convergence test reads.
    /// Infinite for a zero median.
    [[nodiscard]] f64 relative_error() const;
};

/// How bad a warning is, which decides what a caller does about it rather than how it is printed.
enum class nx::bench::warning_severity : nx::u8
{
    /// Worth knowing, and the number stands.
    note,
    /// The number is suspect.
    warning,
    /// The number is meaningless.
    /// A benchmark carrying one of these has failed, not merely wobbled.
    error,
};

/// Something the harness noticed about a run, which a reader would otherwise have to infer from the numbers.
enum class nx::bench::warning_kind : nx::u8
{
    /// The harness's own per-iteration cost is a significant share of what was measured.
    /// The fix is an inner loop and the `void(isize)` body form, which moves the loop inside one timing boundary.
    overhead_significant,

    /// Per-iteration time came in below the calibrated floor, so the body was optimized away.
    /// Not an imprecise number — no number at all.
    body_deleted,

    /// Sampling hit max_time_secs or max_samples without reaching target_relative_error.
    did_not_converge,

    /// A pause/resume pair's two clock reads are a real part of what was measured.
    ///
    /// **Not** "a lot of the wall time was spent paused": a body that pauses around a 10 us refill and measures a
    /// 10 us sort is paused half the time and pays about 14 ns for it, which is nothing.
    /// What matters is the pair's cost against the per-iteration time, and those are different numbers.
    paused_fraction_high,

    /// Fewer samples than the median's interval needs, so ci95 is the sample range rather than a real interval.
    too_few_samples,
};

struct nx::bench::warning
{
    warning_kind kind = warning_kind::did_not_converge;
    warning_severity severity = warning_severity::warning;

    /// What the reader needs beyond the kind: the measured fraction, the floor that was undercut, the sample count.
    cc::string detail;
};

/// One quantity a body recorded per iteration, with what it means.
///
/// The unit is a `cc::rec::unit`, so a reporter knows the symbol, whether it takes binary or decimal prefixes, whether
/// summing or averaging is the right combination, and whether more is good news.
/// That is what lets a latency column and a throughput column sit in one table and colour correctly.
struct nx::bench::recorded_quantity
{
    cc::string name;
    cc::rec::unit const* unit = nullptr;

    /// Combined across every measured iteration, by the unit's own `aggregate`.
    ///
    /// **Within a batch, `aggregate` says how to combine; across batches the rule is derived**, because a unit does
    /// not state one: a summing quantity sums again, and an averaging one becomes a mean of batch means weighted by
    /// iteration count.
    f64 total = 0;

    /// `total` per measured iteration, which is the per-call figure a reader wants.
    f64 per_iteration = 0;

    /// `total` over the measured seconds — the derived rate, free because the harness has both halves.
    /// Zero for an averaging quantity, where a rate is meaningless.
    f64 per_second = 0;
};

/// One hardware counter, measured over a separate pass after the timing was done.
///
/// Counts are far less noisy than times — a retired-instruction count is usually identical run to run — which is why
/// they can decide a comparison in a fraction of the samples timing would need.
/// Less noisy is not exact, though: a cache-miss count moves between runs, and these carry no interval, so read them
/// as a magnitude rather than as a measurement.
struct nx::bench::counter_reading
{
    hw_counter id = {};
    cc::string name;

    /// The raw count over `result::counter_iterations`.
    u64 total = 0;

    f64 per_iteration = 0;

    /// Per item declared through `iteration::items`, or 0 when the body declared none.
    f64 per_item = 0;
};

/// Everything one measured run produced.
///
/// Returned by `nx::bench::run`, which is what makes the harness usable outside a BENCHMARK: inside a run it is
/// reported for you, and anywhere else the caller has the numbers.
struct nx::bench::result
{
    /// The loop's name, empty when it was not given one.
    /// Named loops are what a comparison table keys its rows on.
    cc::string name;

    run_config config;

    /// Per-iteration seconds, one entry per batch, **in the order they were taken**.
    ///
    /// Time order rather than sorted order, on purpose: drift across a run is visible here and nowhere else, and a
    /// consumer that has the samples can recompute any statistic this design got wrong.
    cc::vector<f64> samples;

    /// The statistics of `samples`, in seconds per iteration.
    statistics time;

    isize batch_size = 0;
    isize measured_iterations = 0;
    isize warmup_iterations = 0;
    f64 measured_seconds = 0;

    /// Items the body declared with `iteration::items`, summed over measured iterations.
    /// Zero when it never declared any, which is what distinguishes "no items" from "one item per iteration".
    isize items = 0;

    /// Items per second, or 0 when no items were declared.
    f64 items_per_second = 0;

    cc::vector<recorded_quantity> quantities;

    /// Hardware counters, from passes run AFTER timing converged.
    ///
    /// Empty where the machine has no PMU access, which is the common first experience on a fresh Windows box.
    /// Nothing is read inside a timed region, so the timings are identical whether or not these were available.
    cc::vector<counter_reading> counters;

    /// Iterations the counter passes covered, which is what `counter_reading::total` is over.
    isize counter_iterations = 0;

    cc::vector<warning> warnings;

    /// The harness's own estimated per-iteration cost, as a fraction of the measured per-iteration time.
    f64 overhead_fraction = 0;

    /// The share of the wall time spent paused.
    ///
    /// Reported because it says how much of the loop was setup, which is worth knowing.
    /// It is NOT what the pause warning fires on — see warning_kind::paused_fraction_high.
    f64 paused_fraction = 0;

    /// Whether sampling reached target_relative_error rather than hitting a bound.
    bool converged = false;

    /// True when any warning is an error, which is what a test integration fails on.
    [[nodiscard]] bool has_error() const;

    [[nodiscard]] warning const* find_warning(warning_kind kind) const;
};
