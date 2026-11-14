#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/writer.hh>

// Synthesizes a block out of events copied one by one, for the transforms of the recording algebra.

namespace cc::rec::impl
{
/// Builds one synthesized block out of a sequence of events, taking its metadata from the block they came from.
///
/// The events a transform keeps are no longer contiguous, so they are copied into a buffer of their own.
/// That also means the result stops pinning the chunk it came from, which is usually what a caller narrowing a
/// capture down wanted anyway.
struct block_builder
{
    explicit block_builder(rec::recorded_block const& source) : _template(&source) {}

    void add(rec::event_view const& e)
    {
        auto const header_bytes = isize(sizeof(rec::impl::event_header));
        auto const total = rec::impl::event_bytes_for(e.payload.size());

        auto const offset = _bytes.size();
        _bytes.resize_to_uninitialized(offset + total);

        auto header = rec::impl::event_header{
            .desc = e.desc,
            .cycles = e.cycles,
            .payload_size = u32(e.payload.size()),
            .core = u16(e.core),
            .flags = e.flags,
        };
        cc::memcpy(_bytes.data() + offset, &header, sizeof(header));
        if (!e.payload.empty())
            cc::memcpy(_bytes.data() + offset + header_bytes, e.payload.data(), size_t(e.payload.size()));
        // The padding up to the next event is zeroed, so a synthesized block serializes to the same bytes every time.
        if (auto const pad = total - header_bytes - e.payload.size(); pad > 0)
            cc::memset(_bytes.data() + offset + header_bytes + e.payload.size(), 0, size_t(pad));

        ++_count;
    }

    [[nodiscard]] bool empty() const { return _count == 0; }

    /// Hands back the finished block, leaving the builder empty.
    [[nodiscard]] rec::recorded_block take()
    {
        auto const owned = cc::make_pinned_data(cc::move(_bytes)).reinterpret_as<byte const>();

        auto block = rec::recorded_block{
            .owned = owned,
            .from = 0,
            .to = u32(owned.size()),
            .thread_id = _template->thread_id,
            .thread_index = _template->thread_index,
            .thread_name = _template->thread_name,
            .chunk_seq = _template->chunk_seq,
            .layer = _template->layer,
            .base_cycles = _template->base_cycles,
            .base_wall_secs = _template->base_wall_secs,
            .seal_cycles = _template->seal_cycles,
            .seal_wall_secs = _template->seal_wall_secs,
        };

        _bytes = {};
        _count = 0;
        return block;
    }

private:
    rec::recorded_block const* _template;
    cc::vector<byte> _bytes;
    isize _count = 0;
};
} // namespace cc::rec::impl
