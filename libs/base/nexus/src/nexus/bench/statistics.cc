#include "statistics.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>

#include <limits>

using namespace cc::primitive_defines;

namespace
{
f64 abs_f64(f64 v)
{
    return v < 0 ? -v : v;
}

// A percentile of an ALREADY SORTED vector, by linear interpolation between the two neighbouring ranks.
// Interpolating rather than picking a rank matters at these sample counts: with sixteen samples the quartiles fall
// between entries, and rounding them moves the Tukey fences enough to change the outlier count.
f64 sorted_percentile(cc::span<f64 const> sorted, f64 p)
{
    auto const n = sorted.size();
    CC_ASSERT(n > 0, "a percentile needs at least one sample");
    if (n == 1)
        return sorted[0];

    auto const pos = p * f64(n - 1);
    auto const lo = isize(pos);
    auto const hi = cc::min(lo + 1, n - 1);
    auto const frac = pos - f64(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

f64 sorted_median(cc::span<f64 const> sorted)
{
    return sorted_percentile(sorted, 0.5);
}
} // namespace

isize nx::bench::median_ci_rank(isize n)
{
    if (n < 1)
        return -1;

    // The exact binomial tail, summed from k = 0.
    // Each term is the previous one times (n - k + 1) / k, so nothing overflows and no factorial is ever formed;
    // 2^-n is built by halving rather than by a pow, which keeps it exact and needs no math library.
    auto term = f64(1);
    for (auto i = isize(0); i < n; ++i)
        term *= 0.5; // P(X = 0) = 2^-n

    auto cumulative = term;
    auto best = isize(-1);

    // Wanted is the LARGEST k whose lower tail P(X <= k-1) is still within 2.5%, which is the tightest interval this
    // sample count actually supports.
    for (auto k = isize(1); k <= n / 2; ++k)
    {
        if (cumulative > 0.025)
            break;
        best = k;

        term *= f64(n - k + 1) / f64(k);
        cumulative += term;
    }

    return best;
}

nx::bench::statistics nx::bench::compute_statistics(cc::span<f64 const> samples)
{
    CC_ASSERT(!samples.empty(), "statistics need at least one sample");

    auto sorted = cc::vector<f64>::create_copy_of(samples);
    cc::sort(sorted);

    auto const n = sorted.size();
    auto s = statistics{};
    s.sample_count = n;
    s.min = sorted[0];
    s.max = sorted[n - 1];
    s.median = sorted_median(sorted);
    s.p95 = sorted_percentile(sorted, 0.95);
    s.p99 = sorted_percentile(sorted, 0.99);

    auto sum = f64(0);
    for (auto const v : sorted)
        sum += v;
    s.mean = sum / f64(n);

    // Symmetric 10% trim, with at least one sample surviving however small the vector is.
    {
        auto const cut = isize(f64(n) * 0.1);
        auto const count = n - 2 * cut;
        if (count <= 0)
        {
            s.trimmed_mean = s.median;
        }
        else
        {
            auto trimmed = f64(0);
            for (auto i = cut; i < n - cut; ++i)
                trimmed += sorted[i];
            s.trimmed_mean = trimmed / f64(count);
        }
    }

    {
        auto deviations = cc::vector<f64>::create_defaulted(n);
        for (auto i = isize(0); i < n; ++i)
            deviations[i] = abs_f64(sorted[i] - s.median);
        cc::sort(deviations);
        s.mad = sorted_median(deviations);
    }

    {
        auto const q1 = sorted_percentile(sorted, 0.25);
        auto const q3 = sorted_percentile(sorted, 0.75);
        auto const iqr = q3 - q1;
        auto const low_fence = q1 - 1.5 * iqr;
        auto const high_fence = q3 + 1.5 * iqr;
        for (auto const v : sorted)
            if (v < low_fence || v > high_fence)
                ++s.outliers;
    }

    {
        auto const k = median_ci_rank(n);
        if (k < 0 || k >= n - 1 - k)
        {
            // Too few samples to support any interval: report the range, and say that is what it is.
            s.ci95_low = s.min;
            s.ci95_high = s.max;
            s.ci_is_bound = true;
        }
        else
        {
            s.ci95_low = sorted[k];
            s.ci95_high = sorted[n - 1 - k];
            s.ci_is_bound = false;
        }
    }

    return s;
}

f64 nx::bench::statistics::relative_error() const
{
    if (median <= 0)
        return std::numeric_limits<f64>::infinity();
    return ((ci95_high - ci95_low) * 0.5) / median;
}

bool nx::bench::result::has_error() const
{
    for (auto const& w : warnings)
        if (w.severity == warning_severity::error)
            return true;
    return false;
}

nx::bench::warning const* nx::bench::result::find_warning(warning_kind kind) const
{
    for (auto const& w : warnings)
        if (w.kind == kind)
            return &w;
    return nullptr;
}

nx::bench::recorded_quantity const* nx::bench::result::find_quantity(cc::string_view name) const
{
    for (auto const& q : quantities)
        if (q.name == name)
            return &q;
    return nullptr;
}
