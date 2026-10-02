#include <clean-core/record/log.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/drawing/drawing.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/impl/content_hash.hh>

namespace sv
{
drawing_set_id drawing_manager::acquire(drawing_set const& set)
{
    if (set.cache.manager == this && contains(set.cache.id))
    {
        (void)get(set.cache.id); // the LRU touch a hash hit would have made
        return set.cache.id;
    }

    auto const hash = set.hash();
    auto const resident = find_by_hash(hash);
    auto const id = resident.has_value() ? resident.value() : _place(hash, set.drawings());
    set.cache = {.manager = this, .id = id};
    return id;
}

drawing_set_id drawing_manager::acquire(drawing const& d)
{
    // Seeded apart from a real set's key, so a lone drawing and a set holding only it never alias by accident.
    auto const hash
        = impl::combine_digests(d.hash(), cc::hash128::create(cc::span<byte const>(), impl::drawing_set_hash_seed));
    auto const resident = find_by_hash(hash);
    if (resident.has_value())
        return resident.value();
    return _place(hash, cc::span<drawing const>(&d, 1));
}

u32 drawing_manager::first_record(drawing_set_id id, u32 index)
{
    return get(id).first_record[isize(index)];
}

u32 drawing_manager::record_count(drawing_set_id id, u32 index)
{
    return get(id).record_count[isize(index)];
}

tg::aabb2f drawing_manager::bounds(drawing_set_id id, u32 index)
{
    return get(id).bounds[isize(index)];
}

drawing_set_id drawing_manager::_place(cc::hash128 hash, cc::span<drawing const> drawings)
{
    auto record = drawing_set_record();
    record.first_record.reserve(drawings.size());
    record.record_count.reserve(drawings.size());
    record.bounds.reserve(drawings.size());

    auto bytes = isize(0);
    for (auto const& d : drawings)
    {
        // Each layer is one shape and one record, placed in the drawing's own plane at unit scale: the instance's frame
        // carries the rest.
        auto records = cc::vector<sr::slug_instance>();
        auto failed = false;

        // From every point the curves pass through or are pulled toward, which holds the curves themselves.
        auto lo = tg::pos2f(0, 0);
        auto hi = tg::pos2f(0, 0);
        auto any = false;
        for (auto const& l : d.layers())
            for (auto const& c : l.outline.curves)
                for (auto const& q : {c.p1, c.p2, c.p3})
                {
                    lo = any ? tg::pos2f(cc::min(lo[0], q[0]), cc::min(lo[1], q[1])) : q;
                    hi = any ? tg::pos2f(cc::max(hi[0], q[0]), cc::max(hi[1], q[1])) : q;
                    any = true;
                }
        record.bounds.push_back(tg::aabb2f(lo, hi));

        for (auto const& l : d.layers())
        {
            auto const shape = _atlas.add(sr::compile_slug_shape(l.outline));
            if (shape.has_error())
            {
                failed = true;
                break;
            }
            if (!shape.value().is_drawable)
                continue; // an empty layer has nothing to cover
            records.push_back(
                sr::make_slug_instance(shape.value(), tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), l.color));
        }

        auto first = cc::result<u32>(u32(0));
        if (!failed && !records.empty())
            first = _atlas.add_records(records);
        if (failed || first.has_error())
        {
            if (!_warned_full)
                CC_LOG_WARNING("the drawing atlas is full, so drawings acquired from now on draw nothing");
            _warned_full = true;
            records.clear();
        }

        record.first_record.push_back(first.has_value() ? first.value() : 0);
        record.record_count.push_back(u32(records.size()));
        bytes += records.size() * sr::slug_atlas::texels_per_record * 16;
    }
    return insert(hash, cc::move(record), bytes);
}
} // namespace sv
