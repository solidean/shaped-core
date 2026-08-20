#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/record/desc.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread.hh>

// The consumer side of the wire: how a block of recorded bytes reads back as events.
//
// Nothing here allocates or copies.
// A view borrows the chunk it came from, so it is valid exactly as long as whoever handed it to you says it is —
// inside a listener callback, or for as long as a recording holds its chunks.

/// Which thread a block of events came from.
struct cc::rec::thread_info
{
    cc::thread_id id = cc::thread_id::invalid;
    u32 index = 0;

    /// What cc::rec::set_current_thread_record_name last set, or empty.
    cc::string_view name;
};

/// The stream state as of some point in a thread's event stream.
///
/// Ambient context, the open profiling scopes and the current trace id are stream STATE rather than per-event fields:
/// the producer emits a delta only when one changes, and the consumer carries the running value forward.
/// That is what keeps them off the hot path — see libs/base/clean-core/docs/systems/recording.md.
struct cc::rec::stream_state
{
    /// The cc::async ambient chain head in effect, or null.
    /// Opaque here; cc::async_ambient_lookup_in reads it.
    void* ambient = nullptr;

    /// The trace id in effect, or 0.
    u64 trace_id = 0;

    /// The profiling scopes open at this point, outermost first.
    cc::vector<rec::desc const*> open_scopes;
};

/// One decoded event, borrowing the bytes it came from.
struct cc::rec::event_view
{
    rec::desc const* desc = nullptr;
    u64 cycles = 0;
    u32 core = 0;
    u16 flags = 0;
    cc::span<byte const> payload;

    // the descriptor, restated for readability at call sites
public:
    [[nodiscard]] rec::event_kind kind() const { return desc->kind; }
    [[nodiscard]] rec::level level() const { return desc->lvl; }
    [[nodiscard]] cc::string_view name() const { return desc->name; }
    [[nodiscard]] rec::domain const* domain() const { return desc->dom; }
    [[nodiscard]] rec::unit const* quantity() const { return desc->quantity; }
    [[nodiscard]] rec::source_ref const& site() const { return desc->site; }
    [[nodiscard]] cc::span<rec::field const> fields() const
    {
        return cc::span<rec::field const>(desc->fields, isize(desc->field_count));
    }

    /// True when the payload was cut short because the chunk ran out.
    [[nodiscard]] bool is_truncated() const { return (flags & rec::impl::flag_truncated) != 0; }

    // generic field access, so a consumer that has never heard of the payload can still read it
public:
    /// The named field as a double, for any numeric type.
    /// Empty when there is no such field, or it is not numeric.
    [[nodiscard]] cc::optional<f64> field_as_double(cc::string_view field_name) const;

    /// The named field as a signed integer.
    /// Empty for a non-integral field, and for a u64 past 2^63.
    [[nodiscard]] cc::optional<i64> field_as_int(cc::string_view field_name) const;

    /// The named field as a raw u64, for any unsigned field.
    ///
    /// The accessor an OPAQUE 64-bit value needs: a double loses everything past 2^53, and the signed reader refuses
    /// anything past 2^63, so neither can carry an id that was never a quantity in the first place.
    [[nodiscard]] cc::optional<u64> field_as_u64(cc::string_view field_name) const;

    /// The named field as text, for cstring and inline_text fields.
    [[nodiscard]] cc::optional<cc::string_view> field_as_text(cc::string_view field_name) const;

    /// The named field as a list of u64s, for u64_array fields.
    /// Empty when there is no such field or it is not an array; an array that IS empty is not a case the format can
    /// produce, since a writer with nothing to say writes no event.
    [[nodiscard]] cc::vector<u64> field_as_u64_array(cc::string_view field_name) const;

    /// What a trace_relation site's edge means, or null for every other kind.
    [[nodiscard]] rec::relation_type const* relation() const
    {
        return kind() == rec::event_kind::trace_relation ? desc->relation : nullptr;
    }

    /// The whole payload read as inline text, which is how a formatted log message is stored.
    [[nodiscard]] cc::string_view payload_as_text() const
    {
        return cc::string_view(reinterpret_cast<char const*>(payload.data()), payload.size());
    }
};

/// Walks the events in a block of recorded bytes.
///
/// The block must start on an event boundary and end on one, which is exactly what a chunk's committed prefix is.
struct cc::rec::event_iterator
{
    event_iterator() = default;
    explicit event_iterator(cc::span<byte const> bytes) : _cur(bytes.data()), _end(bytes.data() + bytes.size()) {}

    [[nodiscard]] rec::event_view operator*() const;

    event_iterator& operator++();

    [[nodiscard]] bool operator!=(cc::sentinel) const { return _cur < _end; }
    [[nodiscard]] bool operator==(cc::sentinel) const { return _cur >= _end; }

private:
    byte const* _cur = nullptr;
    byte const* _end = nullptr;
};

/// A block of one thread's events, with everything needed to interpret them.
///
/// This is what a listener is handed, and what a recording is made of.
/// `state_at_start` is null only for a chunk the consumer never reached — the tail of a crash dump.
struct cc::rec::chunk_view
{
    rec::chunk const* source = nullptr;
    rec::thread_info thread;
    rec::stream_state const* state_at_start = nullptr;
    cc::span<byte const> bytes;

    /// Where this block sits in its thread's sequence, so a hole between two blocks is detectable.
    u64 chunk_seq = 0;

    /// The listener layer these events were recorded under, or chunk::no_layer for ordinary code.
    u16 layer = 0xFFFF;

    /// The (cycles, wall) pairs bracketing the source chunk, for turning a cycle count into a timestamp.
    /// `seal_wall_secs` is zero while the chunk is still being written, in which case only the base pair is known.
    u64 base_cycles = 0;
    f64 base_wall_secs = 0;
    u64 seal_cycles = 0;
    f64 seal_wall_secs = 0;

    [[nodiscard]] rec::event_iterator begin() const { return rec::event_iterator(bytes); }
    [[nodiscard]] cc::sentinel end() const { return {}; }

    /// Converts a cycle reading in this block to seconds since the Unix epoch, interpolating between the two pairs.
    /// Falls back to the base pair alone — and therefore to a cycle rate estimated elsewhere — on a live chunk.
    [[nodiscard]] f64 wall_secs_of(u64 cycles) const;
};
