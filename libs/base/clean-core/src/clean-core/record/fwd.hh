#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

// The recording system's vocabulary: what an event can be, and the types the rest of record/ names.
//
// One event stream carries logging, profiling, values, stats and tracing.
// They differ only in their descriptor, never in the path that writes them — see libs/base/clean-core/docs/systems/recording.md.

namespace cc::rec
{
//
// Enumerations
//

enum class event_kind : u8;
enum class level : u8;
enum class category : u8;
enum class type_code : u8;
enum class overflow_policy : u8;
enum class aggregation : u8;
enum class axis_scale : u8;
} // namespace cc::rec

/// What an event IS.
/// The producer-written kinds come first; everything from stream_state on is written by the consumer or by the
/// system itself, never by a user macro.
enum class cc::rec::event_kind : cc::u8
{
    invalid = 0,

    log,             ///< a message, formatted on the calling thread
    scope_begin,     ///< a thread-local profiling scope opens
    scope_end,       ///< ... and closes; the payload carries the depth it closed at
    marker,          ///< "this code ran", with no value attached
    value,           ///< an arbitrary user value, laid out by the descriptor's fields
    stat_snapshot,   ///< the current reading of a quantity (queue depth, resident bytes)
    stat_accumulate, ///< a delta to add up (bytes uploaded, tasks finished)
    ambient_changed, ///< the thread adopted a different cc::async ambient context
    trace_relation,  ///< two trace ids are related; the graph is reconstructed offline
    trace_scope,     ///< the thread entered or left a trace id

    stream_state,   ///< the consumer-written preamble that makes a chunk independently decodable
    gap,            ///< events were dropped; carries how many, over what span, and how many bytes
    chunk_acquired, ///< the cold path ran, and how long it took
    late_event,     ///< an event surfaced below an ordered listener's emitted watermark
    dropped_span,   ///< a listener decimated a time span away, and knows it

    count,
};

/// How important a log message is.
/// Only kind == log carries one; every other kind gates on its category instead.
enum class cc::rec::level : cc::u8
{
    trace = 0,
    debug,
    info,
    warning,
    error,

    count,
};

/// The gating axis for everything that is not a log message.
/// A domain can silence its profiling without silencing its errors, which is what makes always-on annotation affordable.
enum class cc::rec::category : cc::u8
{
    logging = 0,
    profiling,
    values,
    stats,
    tracing,

    count,
};

/// How a payload field is laid out, so a consumer that has never heard of the type can still print and compare it.
enum class cc::rec::type_code : cc::u8
{
    none = 0,
    boolean,
    i8_,
    i16_,
    i32_,
    i64_,
    u8_,
    u16_,
    u32_,
    u64_,
    f32_,
    f64_,
    pointer,      ///< an opaque address, printed as hex and never dereferenced
    cstring,      ///< a char const* with static lifetime, stored as the pointer
    inline_text,  ///< a u32 length followed by that many bytes, inline in the payload
    pinned_bytes, ///< the bytes live behind a pin; the payload holds the pin index and the span

    count,
};

/// What happens when a thread's chunk is full and the pool cannot hand out another.
enum class cc::rec::overflow_policy : cc::u8
{
    /// Drop events until the consumer frees a chunk, accounting the loss in a gap event.
    /// The release default: recording must never change the timing of what it measures.
    drop,
    /// Block the producer until a chunk frees up.
    /// The dev default, so nothing goes missing while you are looking for it, at the cost of perturbing the timing.
    backpressure,
    /// Grow past the budget without bound.
    /// The test default, where the whole run is expected to fit and completeness is the point.
    grow_unbounded,
};

/// How a stat's samples combine over a time bucket.
enum class cc::rec::aggregation : cc::u8
{
    sum,
    mean,
    last,
    maximum,
    minimum,
};

/// Whether a stat's natural axis is linear or logarithmic.
enum class cc::rec::axis_scale : cc::u8
{
    linear,
    logarithmic,
};

namespace cc::rec
{


//
// Types
//

struct domain; // also declared in domain_fwd.hh, which fwd.hh headers include instead of this one
struct field;
struct unit;
struct desc;

struct pin;
struct chunk;
struct chunk_ref;
struct chunk_pool;

struct config;
struct system_stats;
struct stream_state;
struct thread_info;

struct event_writer;
struct event_view;
struct chunk_view;
struct event_iterator;

struct listener;
struct listener_handle;
struct console_listener;
struct console_options;
template <class Derived>
struct event_listener;

struct recorded_block;
struct recording;
struct recording_listener;
struct scope_span;
struct decimation_options;

namespace impl
{
struct writer_tls;
struct thread_state;
struct event_header;
struct listener_registry;
struct listener_layer_scope;
} // namespace impl


//
// Bit helpers for domain gating
//

/// The enabled_mask bit a log message of `l` gates on.
[[nodiscard]] constexpr u32 enable_bit_of(rec::level l)
{
    return u32(1) << u32(l);
}

/// The enabled_mask bit everything that is not a log message gates on.
/// Categories live in the high half so the two axes never collide.
[[nodiscard]] constexpr u32 enable_bit_of(rec::category c)
{
    return u32(1) << (16 + u32(c));
}

/// Every level bit, for building a mask.
inline constexpr u32 all_level_bits = (u32(1) << u32(level::count)) - 1;

/// Every category bit, for building a mask.
inline constexpr u32 all_category_bits = ((u32(1) << u32(category::count)) - 1) << 16;
} // namespace cc::rec
