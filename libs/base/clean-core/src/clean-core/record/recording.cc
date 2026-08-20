#include "recording.hh"

#include <clean-core/common/utility.hh>

using namespace cc::primitive_defines;

cc::rec::chunk_view cc::rec::recorded_block::view() const
{
    return {
        .source = source.get(),
        .thread = {.id = thread_id, .index = thread_index, .name = thread_name},
        .state_at_start = state_at_start,
        .bytes = cc::span<byte const>(source.get()->data + from, isize(to - from)),
        .chunk_seq = chunk_seq,
        .layer = layer,
        .base_cycles = base_cycles,
        .base_wall_secs = base_wall_secs,
        .seal_cycles = seal_cycles,
        .seal_wall_secs = seal_wall_secs,
    };
}

void cc::rec::recording::append(cc::rec::chunk_view const& view)
{
    if (view.source == nullptr || view.bytes.empty())
        return;

    auto* const c = const_cast<rec::chunk*>(view.source);
    auto const offset = u32(view.bytes.data() - c->data);

    _blocks.push_back({
        .source = rec::chunk_ref(c),
        .from = offset,
        .to = offset + u32(view.bytes.size()),
        .thread_id = view.thread.id,
        .thread_index = view.thread.index,
        .thread_name = cc::string(view.thread.name),
        .chunk_seq = view.chunk_seq,
        .layer = view.layer,
        .base_cycles = view.base_cycles,
        .base_wall_secs = view.base_wall_secs,
        .seal_cycles = view.seal_cycles,
        .seal_wall_secs = view.seal_wall_secs,
        .state_at_start = view.state_at_start,
    });
}

void cc::rec::recording::append(cc::rec::recording const& other)
{
    for (auto const& b : other._blocks)
        _blocks.push_back(b);
}

bool cc::rec::recording::empty() const
{
    for (auto const& b : _blocks)
        if (b.to > b.from)
            return false;
    return true;
}

isize cc::rec::recording::event_count() const
{
    isize n = 0;
    for (auto const& b : _blocks)
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            ++n;
    }
    return n;
}

void cc::rec::recording::for_each_event(cc::function_ref<void(cc::rec::chunk_view const&, cc::rec::event_view const&)> f) const
{
    for (auto const& b : _blocks)
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            f(v, *it);
    }
}

void cc::rec::recording::replay(cc::rec::listener& l) const
{
    for (auto const& b : _blocks)
        l.on_chunk(b.view());
    l.on_batch_end();
}

cc::rec::recording cc::rec::recording_listener::take()
{
    return cc::move(_recording);
}
