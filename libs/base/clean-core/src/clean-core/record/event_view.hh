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
    /// The site's name: the descriptor's, or the payload's for a CC_RECORD_NAMED site.
    ///
    /// The dynamic case is why this is not a plain member read — a site whose name is only known at runtime leaves the
    /// descriptor's empty and carries the name in its payload, and every query keyed on a name has to see the same
    /// answer either way.
    [[nodiscard]] cc::string_view name() const;
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

    /// The named field as another site's descriptor, for `desc_ref` fields.
    ///
    /// Null when there is no such field, when it is not a descriptor reference, or when it names nothing — which is
    /// what a chunk preamble's unused scope slots hold.
    /// Valid for as long as the recording is: on a loaded one it points into that `loaded_recording`'s own table.
    [[nodiscard]] rec::desc const* field_as_desc(cc::string_view field_name) const;

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

    /// Where in the block this iterator sits, for a consumer that slices a block rather than reading it.
    [[nodiscard]] byte const* position() const { return _cur; }

private:
    byte const* _cur = nullptr;
    byte const* _end = nullptr;
};

/// A block of one thread's events, with everything needed to interpret them.
///
/// This is what a listener is handed, and what a recording is made of.
///
/// **The stream state is not a field here — it is the chunk's first EVENT.**
/// Every chunk opens with an `event_kind::stream_state` naming the trace and the scopes that were already open, so a
/// block is decodable from its own bytes with nothing carried in alongside it.
/// A block that is a later SLICE of a chunk does not repeat it, so a reader that slices carries the value forward the
/// way it carries any other state.
struct cc::rec::chunk_view
{
    rec::chunk const* source = nullptr;
    rec::thread_info thread;
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
