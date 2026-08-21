#include "stack_table.hh"

#include <clean-core/record/sampling.hh>

using namespace cc::primitive_defines;

cc::rec::stack_table::stack_table(rec::recording const& r)
{
    r.for_each_event(
        [&](rec::chunk_view const&, rec::event_view const& e)
        {
            // By KIND rather than by descriptor: a recording loaded from a file has descriptors of its own, so
            // comparing addresses would find nothing in exactly the case this exists for.
            if (e.desc->kind != rec::event_kind::stack_definition)
                return;

            auto const id = e.field_as_u64("id");
            if (!id.has_value() || id.value() == 0)
                return;

            _stacks[id.value()] = e.field_as_u64_array("frames");
        });
}

cc::span<u64 const> cc::rec::stack_table::frames_of_id(u64 id) const
{
    auto const* const hit = _stacks.get_ptr(id);
    return hit != nullptr ? cc::span<u64 const>(*hit) : cc::span<u64 const>();
}

cc::vector<u64> cc::rec::stack_table::frames_of(rec::event_view const& e) const
{
    auto frames = e.field_as_u64_array("frames");

    if (!e.has_interned_stack())
        return frames;

    // An interned sample carries exactly one entry, and that entry is the id.
    // Anything else is a recording whose sampler and reader disagree, and returning nothing is better than returning
    // an address that is really a counter.
    if (frames.size() != 1)
        return {};

    cc::vector<u64> out;
    out.push_back_range(frames_of_id(frames.front()));
    return out;
}

isize cc::rec::stack_table::total_frames() const
{
    isize n = 0;
    for (auto const& [id, frames] : _stacks)
        n += frames.size();
    return n;
}
