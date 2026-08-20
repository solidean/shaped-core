#pragma once

#include <clean-core/record/record.hh>

// CC_RECORD_STAT and CC_RECORD_ACCUM — quantities a listener can graph without ever having heard of them.
//
// The two kinds are not interchangeable, and picking the wrong one produces a plausible graph of the wrong thing.
// A SNAPSHOT is the current reading of something that exists whether or not you look: queue depth, resident bytes,
// frame time.
// Summing snapshots is meaningless; averaging them is not.
// An ACCUMULATE is a delta to add up: bytes uploaded, tasks finished, cache misses.
// Summing them is the whole point.
//
// Values are `f64` only, which also covers every integer up to 2^53.
// That is a deliberate cap: one numeric type means a listener can graph anything without a type switch.

namespace cc::rec
{
/// What a stat MEANS, as a static object the event points at.
///
/// Deliberately a struct rather than an enum: everyone's enum of units is missing the case the next consumer needs,
/// and adding a member here breaks nobody where adding an enumerator forces every switch to be revisited.
/// Only analysis ever reads one, so the whole cost is a pointer in the descriptor.
///
/// Define your own next to the code that records it; these are only the ones that come up everywhere.
inline constexpr rec::unit unit_count = {
    .singular = "item",
    .plural = "items",
    .symbol = "",
    .prefix_base = 1000,
    .aggregate = rec::aggregation::sum,
};

inline constexpr rec::unit unit_bytes = {
    .singular = "byte",
    .plural = "bytes",
    .symbol = "B",
    .prefix_base = 1024,
    .aggregate = rec::aggregation::sum,
};

inline constexpr rec::unit unit_seconds = {
    .singular = "second",
    .plural = "seconds",
    .symbol = "s",
    .prefix_base = 1000,
    .aggregate = rec::aggregation::sum,
};

inline constexpr rec::unit unit_ratio = {
    .singular = "ratio",
    .plural = "ratios",
    .symbol = "",
    .prefix_base = 0,
    .aggregate = rec::aggregation::mean,
    .default_min = 0,
    .default_max = 1,
    .higher_is_better = true,
};

inline constexpr rec::unit unit_hertz = {
    .singular = "hertz",
    .plural = "hertz",
    .symbol = "Hz",
    .prefix_base = 1000,
    .aggregate = rec::aggregation::mean,
    .higher_is_better = true,
};
} // namespace cc::rec

namespace cc::rec::impl
{
/// The layout every stat event shares, so one graphing listener covers all of them.
inline constexpr rec::field stat_fields[] = {
    {.name = "value", .type = rec::type_code::f64_, .offset = 0, .size = 8},
};
} // namespace cc::rec::impl

/// Records the CURRENT reading of a quantity: queue depth, resident bytes, frame time.
/// Summing snapshots is meaningless — reach for CC_RECORD_ACCUM when adding up is what you want.
#define CC_RECORD_STAT(name_, unit_, value_)                                                                \
    do                                                                                                      \
    {                                                                                                       \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::stat_snapshot, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::stats), (name_), &(unit_),         \
                           ::cc::rec::impl::stat_fields, 1, 8);                                             \
        ::cc::rec::record_event(cc_rec_site_desc_, ::cc::f64(value_));                                      \
    } while (false)

/// Records a DELTA to add up: bytes uploaded, tasks finished, cache misses.
#define CC_RECORD_ACCUM(name_, unit_, delta_)                                                                 \
    do                                                                                                        \
    {                                                                                                         \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::stat_accumulate, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::stats), (name_), &(unit_),           \
                           ::cc::rec::impl::stat_fields, 1, 8);                                               \
        ::cc::rec::record_event(cc_rec_site_desc_, ::cc::f64(delta_));                                        \
    } while (false)
