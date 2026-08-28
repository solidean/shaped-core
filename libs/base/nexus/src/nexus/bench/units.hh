#pragma once

#include <clean-core/record/desc.hh>

// The units a benchmark reports in, which clean-core's own set does not cover.
//
// `cc::rec::unit_bytes` describes bytes; bytes PER SECOND is a different quantity with a different orientation — more
// bytes is neutral, more bytes per second is good news.
// clean-core/record/stat.hh says to define your own next to the code that records it, and rates are a benchmarking
// concept, so they live here.
//
// Every one of these carries its own `higher_is_better`, which is what lets a report colour a column and a PGO
// comparison read a delta correctly without the caller restating it.

namespace nx::bench
{
/// Throughput in bytes per second, in binary prefixes (KiB/s, MiB/s, GiB/s).
inline constexpr cc::rec::unit unit_bytes_per_second = {
    .singular = "byte per second",
    .plural = "bytes per second",
    .symbol = "B/s",
    .prefix_base = 1024,
    .aggregate = cc::rec::aggregation::mean,
    .higher_is_better = true,
};

/// Throughput in things per second — elements, operations, allocations — in decimal prefixes.
inline constexpr cc::rec::unit unit_items_per_second = {
    .singular = "per second",
    .plural = "per second",
    .symbol = "/s",
    .prefix_base = 1000,
    .aggregate = cc::rec::aggregation::mean,
    .higher_is_better = true,
};

/// Seconds per item — the latency of one operation, where less is better.
///
/// Distinct from `cc::rec::unit_seconds`, which is a duration that SUMS; this one is a per-operation average and
/// averaging is how several of them combine.
inline constexpr cc::rec::unit unit_seconds_per_item = {
    .singular = "second per item",
    .plural = "seconds per item",
    .symbol = "s/item",
    .prefix_base = 1000,
    .aggregate = cc::rec::aggregation::mean,
    .higher_is_better = false,
};

/// A share of some budget, 0..1, where less is better — the fraction of a loop spent in one phase, say.
///
/// Distinct from `cc::rec::unit_ratio`, which is the same range read the other way round: a hit rate is good news
/// when it rises, and a cost share is not.
inline constexpr cc::rec::unit unit_cost_share = {
    .singular = "share",
    .plural = "shares",
    .symbol = "",
    .prefix_base = 0,
    .aggregate = cc::rec::aggregation::mean,
    .default_min = 0,
    .default_max = 1,
    .higher_is_better = false,
};

/// A ratio against a baseline where MORE IS WORSE: 2 means twice the cost.
///
/// The mirror of unit_speedup, and separate from it for the same reason unit_cost_share is separate from
/// cc::rec::unit_ratio — the number reads identically and means the opposite, so only the unit can say which.
inline constexpr cc::rec::unit unit_overhead = {
    .singular = "times",
    .plural = "times",
    .symbol = "x",
    .prefix_base = 0,
    .aggregate = cc::rec::aggregation::mean,
    .higher_is_better = false,
};

/// A ratio against a baseline: 2 means twice as fast.
/// No prefix at all — a speedup of 1.2 k would be a benchmark that is wrong rather than fast.
inline constexpr cc::rec::unit unit_speedup = {
    .singular = "times",
    .plural = "times",
    .symbol = "x",
    .prefix_base = 0,
    .aggregate = cc::rec::aggregation::mean,
    .higher_is_better = true,
};
} // namespace nx::bench
