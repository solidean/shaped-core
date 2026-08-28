#include <clean-core/container/vector.hh>
#include <nexus/bench/statistics.hh>
#include <nexus/test.hh>

// The statistics, against vectors whose answers are worked out by hand.
//
// Every number here is exact and reproducible, which is the argument for the order-statistic interval over a bootstrap:
// there is no seed, so a test can assert an interval rather than a tolerance around one.

using namespace cc::primitive_defines;

namespace
{
// 1..10, whose every statistic is checkable on paper.
cc::vector<f64> one_to_ten()
{
    auto v = cc::vector<f64>();
    for (auto i = 1; i <= 10; ++i)
        v.push_back(f64(i));
    return v;
}
} // namespace

TEST("bench - the tail percentiles are order statistics of the samples")
{
    // 0..100 inclusive, so a percentile lands exactly on its own value and nothing is interpolated.
    auto samples = cc::vector<f64>();
    for (auto i = 0; i <= 100; ++i)
        samples.push_back(f64(i));

    auto const s = nx::bench::compute_statistics(samples);

    CHECK(s.p95 == 95.0);
    CHECK(s.p99 == 99.0);
    CHECK(s.max == 100.0);

    // A latency target is written against the tail, so it must not be the median wearing another name.
    CHECK(s.median == 50.0);
}

TEST("bench - statistics of a known vector")
{
    auto const v = one_to_ten();
    auto const s = nx::bench::compute_statistics(v);

    CHECK(s.sample_count == 10);
    CHECK(s.min == 1.0);
    CHECK(s.max == 10.0);

    // n is even, so the median falls between 5 and 6 and is interpolated rather than rounded to a rank.
    CHECK(s.median == 5.5);
    CHECK(s.mean == 5.5);

    // A 10% trim drops one sample from each end, leaving 2..9 — whose mean is also 5.5.
    CHECK(s.trimmed_mean == 5.5);

    // Deviations from 5.5 are {0.5, 0.5, 1.5, 1.5, 2.5, 2.5, 3.5, 3.5, 4.5, 4.5}; their median is 2.5.
    CHECK(s.mad == 2.5);

    // q1 = 3.25, q3 = 7.75, iqr = 4.5, so the fences are far outside the data.
    CHECK(s.outliers == 0);
}

TEST("bench - the median interval is the order statistics, exactly")
{
    auto const v = one_to_ten();
    auto const s = nx::bench::compute_statistics(v);

    // n = 10: P(X <= 1) = 11/1024 is within 2.5% and P(X <= 2) = 56/1024 is not, so k = 2.
    CHECK(nx::bench::median_ci_rank(10) == 2);
    CHECK(s.ci95_low == 3.0);  // sorted[2]
    CHECK(s.ci95_high == 8.0); // sorted[10 - 1 - 2]
    CHECK(!s.ci_is_bound);
}

TEST("bench - median_ci_rank against hand-computed binomial tails")
{
    // No k at all below six samples: even P(X <= 0) = 1/32 exceeds 2.5% at n = 5.
    CHECK(nx::bench::median_ci_rank(1) == -1);
    CHECK(nx::bench::median_ci_rank(5) == -1);

    // n = 6: P(X <= 0) = 1/64 = 0.0156 fits, P(X <= 1) = 7/64 = 0.109 does not.
    CHECK(nx::bench::median_ci_rank(6) == 1);

    // n = 16: P(X <= 3) = 697/65536 = 0.0106 fits, P(X <= 4) = 2517/65536 = 0.0384 does not.
    CHECK(nx::bench::median_ci_rank(16) == 4);

    // The interval never inverts, however large n gets.
    for (auto n = isize(6); n <= 256; ++n)
    {
        auto const k = nx::bench::median_ci_rank(n);
        CHECK(k >= 1);
        CHECK(k < n - 1 - k);
    }
}

TEST("bench - too few samples reports the range rather than an interval")
{
    auto v = cc::vector<f64>{1.0, 2.0, 3.0, 4.0, 5.0};
    auto const s = nx::bench::compute_statistics(v);

    CHECK(s.ci_is_bound);
    CHECK(s.ci95_low == 1.0);
    CHECK(s.ci95_high == 5.0);
}

TEST("bench - outliers are counted and never dropped")
{
    auto v = cc::vector<f64>{1.0, 1.0, 1.0, 1.0, 100.0};
    auto const s = nx::bench::compute_statistics(v);

    // q1 = q3 = 1, so the fences collapse onto the value and 100 sits outside them.
    CHECK(s.outliers == 1);

    // Counted, not removed: the mean still carries the excursion, which is what makes it visible.
    CHECK(s.sample_count == 5);
    CHECK(s.max == 100.0);
    CHECK(s.mean > 20.0);
    CHECK(s.median == 1.0);
}

TEST("bench - a single sample degrades without dividing by zero")
{
    auto v = cc::vector<f64>{4.0};
    auto const s = nx::bench::compute_statistics(v);

    CHECK(s.sample_count == 1);
    CHECK(s.median == 4.0);
    CHECK(s.mean == 4.0);
    CHECK(s.min == 4.0);
    CHECK(s.max == 4.0);
    CHECK(s.mad == 0.0);
    CHECK(s.ci_is_bound);
}

TEST("bench - relative_error is the half-width over the median")
{
    auto v = cc::vector<f64>();
    for (auto i = 0; i < 20; ++i)
        v.push_back(10.0);

    auto const s = nx::bench::compute_statistics(v);
    CHECK(s.median == 10.0);
    CHECK(s.relative_error() == 0.0); // every sample identical, so the interval has no width at all
}
