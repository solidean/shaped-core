#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-viewer/drawing/drawing.hh>
#include <shaped-viewer/impl/content_hash.hh>

namespace sv
{
drawing& drawing::add_fill(sv::path const& p, fill_style const& style)
{
    return add_fill(p.to_outline(style.rule), style);
}

drawing& drawing::add_fill(sr::slug_outline outline, fill_style const& style)
{
    CC_ASSERT(outline.is_closed(), "a drawing's outline must be closed: call close() after each contour");
    outline.fill_rule = style.rule;
    _layers.push_back({.outline = cc::move(outline), .color = style.color});
    _hash_dirty = true;
    return *this;
}

drawing& drawing::add_stroke(sv::path const& p, stroke_style const& style)
{
    auto const box = p.bounds();
    auto const extent = cc::max(box.max[0] - box.min[0], box.max[1] - box.min[1]) + style.width;
    auto outline = sr::stroke_outline(p,
                                      {.width = style.width,
                                       .join = style.join,
                                       .cap = style.cap,
                                       .miter_limit = style.miter_limit,
                                       .dashes = style.dashes,
                                       .dash_offset = style.dash_offset},
                                      extent / 4096.0f);
    if (outline.is_empty())
        return *this;
    return add_fill(cc::move(outline), {.color = style.color, .rule = sr::slug_fill_rule::nonzero});
}

drawing& drawing::add_drawing(drawing const& d, frame_2d const& frame)
{
    auto const x_axis = frame.x_axis * frame.scale;
    auto const y_axis = frame.y_axis * frame.scale;
    auto const place = [&](tg::pos2f p) { return frame.at + x_axis * p[0] + y_axis * p[1]; };
    for (auto const& l : d.layers())
    {
        auto outline = l.outline;
        for (auto& c : outline.curves)
            c = {.p1 = place(c.p1), .p2 = place(c.p2), .p3 = place(c.p3)};
        auto const color = tg::vec4f(l.color[0] * frame.tint[0], l.color[1] * frame.tint[1], l.color[2] * frame.tint[2],
                                     l.color[3] * frame.tint[3]);
        _layers.push_back({.outline = cc::move(outline), .color = color});
    }
    _hash_dirty = true;
    return *this;
}

cc::hash128 drawing::hash() const
{
    if (!_hash_dirty)
        return _hash;

    // One digest per layer part, folded in order: the curves and contour ends are separate allocations, and the rule
    // and colour decide the pixels as much as the geometry does.
    auto h = cc::hash128::create(cc::span<byte const>(), impl::drawing_hash_seed);
    for (auto const& l : _layers)
    {
        h = impl::combine_digests(h, cc::hash128::create(cc::span<sr::slug_curve const>(l.outline.curves).as_bytes(),
                                                         impl::drawing_hash_seed));
        h = impl::combine_digests(
            h, cc::hash128::create(cc::span<i32 const>(l.outline.contour_ends).as_bytes(), impl::drawing_hash_seed));
        f32 const tail[] = {f32(l.outline.fill_rule), l.color[0], l.color[1], l.color[2], l.color[3]};
        h = impl::combine_digests(h, cc::hash128::create(cc::span<f32 const>(tail).as_bytes(), impl::drawing_hash_seed));
    }
    _hash = h;
    _hash_dirty = false;
    return _hash;
}

drawing_id drawing_set::add(drawing d)
{
    _drawings.push_back(cc::move(d));
    _hash_dirty = true;
    return drawing_id(u32(_drawings.size() - 1));
}

drawing const& drawing_set::operator[](drawing_id id) const
{
    CC_ASSERT(u32(id) < u32(_drawings.size()), "a drawing_id names a drawing of the set that minted it");
    return _drawings[isize(id)];
}

cc::hash128 drawing_set::hash() const
{
    if (!_hash_dirty)
        return _hash;

    auto digests = cc::vector<cc::hash128>();
    digests.reserve(_drawings.size());
    for (auto const& d : _drawings)
        digests.push_back(d.hash());
    _hash = cc::hash128::create(cc::span<cc::hash128 const>(digests).as_bytes(), impl::drawing_set_hash_seed);
    _hash_dirty = false;
    return _hash;
}
} // namespace sv
