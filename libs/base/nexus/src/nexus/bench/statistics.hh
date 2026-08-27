#pragma once

#include <clean-core/container/span.hh>
#include <nexus/bench/fwd.hh>
#include <nexus/bench/result.hh>

// Turning a sample vector into something honest.
//
// Nothing here is specific to timing: the input is a vector of observations and the output says where their centre is
// and how much of it to believe.
// Split out from the run engine because that is what makes it testable against a known vector, which is the only way
// to be sure a statistic is right.

namespace nx::bench
{
/// Every statistic of `samples`, which must be non-empty.
///
/// **Deterministic**: the same vector always produces the same answer, on every platform, with no seed anywhere.
/// That is the argument for the order-statistic interval over a bootstrap — see statistics::ci95_low.
///
/// `samples` is not modified; the sort happens on a copy.
[[nodiscard]] statistics compute_statistics(cc::span<f64 const> samples);

/// The 2.5% order-statistic rank for a 95% interval on the median of `n` samples.
///
/// Returns the largest k for which `P(Bin(n, 0.5) <= k-1) <= 0.025`, so `[x(k), x(n-1-k)]` covers the median with at
/// least 95% probability.
/// Returns -1 when no such k exists, which is every n below six: there is no interval that small a sample supports,
/// and saying so is the point.
[[nodiscard]] isize median_ci_rank(isize n);
} // namespace nx::bench
