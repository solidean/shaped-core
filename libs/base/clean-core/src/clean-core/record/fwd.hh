#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

// The recording system's vocabulary: what an event can be, and the types the rest of record/ names.
//
// One event stream carries logging, profiling, values, stats and tracing.
// They differ only in their descriptor, never in the path that writes them — see libs/base/clean-core/docs/systems/recording.md.

namespace cc::rec
{
/// The recorder's own bookkeeping answers to the system domain rather than to clean-core's.
///
/// This shadows `cc::g_rec_domain` for everything inside cc::rec, so silencing `cc` never blinds you to how the
/// recorder itself is doing — the two are separate questions and gate separately.
CC_REC_DECLARE_DOMAIN(g_system_domain);

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
enum class trace_id : u64;
} // namespace cc::rec

/// What an event IS.
/// The producer-written kinds come first; everything from stream_state on is written by the recorder itself rather
/// than by a user macro.
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
    trace_relation,  ///< some trace ids are related; the graph is reconstructed offline

    /// An async scope was declared, naming it and binding it to its trace id.
    /// The pair brackets the SCOPE OBJECT's life, not the work under it — that outlives the scope by design.
    async_scope_begin,
    async_scope_end,

    /// A sampler caught a thread mid-work: an anchor into that thread's stream, plus the frames it was in.
    /// Written to the SAMPLER's stream, never the sampled thread's — see record/sampling.hh.
    sample,

    /// The preamble every chunk opens with: the trace in effect, and the scopes already open.
    ///
    /// Written by the PRODUCER, at rotation, because none of it can be derived later.
    /// A long-lived scope opens once and never re-opens, so a window that outlived its `scope_begin` — a ring buffer,
    /// a crash dump's tail, a decimated capture — has no other way to know it is inside one.
    stream_state,
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
    pointer,     ///< an opaque address, printed as hex and never dereferenced
    cstring,     ///< a char const* with static lifetime, stored as the pointer
    inline_text, ///< a u32 length followed by that many bytes, inline in the payload
    /// Bytes the chunk keeps alive rather than copies: the payload holds an address and a size.
    ///
    /// **Only dereferenceable while the recording that owns the chunk is alive**, which is what the pin buys.
    /// Serializing copies the bytes into a blob section, so a loaded recording's pointer names its own storage — read
    /// one through `event_view::field_as_bytes` rather than by casting the address.
    pinned_bytes,
    u64_array, ///< a u32 count at the field's offset, then that many u64s starting four bytes later

    /// A `rec::desc const*` stored in eight bytes, naming another recording site.
    ///
    /// The one payload type that is a POINTER INTO THIS BINARY and still survives a file: serializing rewrites it into
    /// the descriptor table's index and loading rewrites it back, exactly as an event header's own descriptor is.
    /// Anything else pointing at process memory has to be copied into the payload instead.
    desc_ref,

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

/// A correlation id, unique within the process.
///
/// Deliberately opaque and deliberately not registered anywhere: minting one is a counter increment, and an id that
/// nothing tracks cannot leak, cannot be looked up wrongly, and costs nothing to abandon.
enum class cc::rec::trace_id : cc::u64
{
    none = 0,
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
struct source_ref;
struct field;
struct unit;
struct relation_type;
struct desc;

struct pin;

/// Opts a type in to being recorded by pin rather than by copy; see record/pinned_value.hh.
template <class T>
struct pinnable_traits;

struct chunk;
struct chunk_ref;
struct chunk_pool;

struct config;
struct crash_dump_options;
struct overhead_model;
struct system_stats;
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
struct loaded_recording;
struct scope_span;
struct trace_relation;
struct decimation_options;
struct retention_policy;
struct sampling_override;
struct splice_options;
struct splicing_listener;

namespace impl
{
struct writer_tls;
struct thread_state;
struct event_header;
struct listener_registry;
struct recording_loader;
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
