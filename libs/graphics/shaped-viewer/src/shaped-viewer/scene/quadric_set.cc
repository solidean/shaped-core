#include "quadric_set.hh"

#include <clean-core/common/assert.hh>
#include <shaped-viewer/impl/content_hash.hh>

using namespace cc::primitive_defines;


void sv::quadric_set::add(quadric_primitive const& p)
{
    // The hash is deferred rather than folded: one streaming pass over the span beats 2N short-input calls by about
    // five times for the same invariant, and nothing here asks for the key until the set is placed.
    _hash_dirty = true;

    // The bounds fold stays, because a union of boxes is not a byte range and has no bulk form to defer to.
    CC_ASSERT(!p.bounds.is_empty(), "a quadric primitive needs its bounds: the BLAS reads them as its box");
    _bounds = _bounds.has_value() ? tg::aabb_of(_bounds.value(), p.bounds) : p.bounds;

    _primitives.push_back(p);
}

cc::hash128 sv::quadric_set::hash() const
{
    if (_hash_dirty)
    {
        // Over the raw bytes of the whole span, which is what makes the result order-sensitive without arranging for it.
        // `quadric_primitive` is padding-free and static_asserts that it is — see scene/quadric.hh — so no indeterminate
        // byte ever reaches a cache key.
        _hash = cc::hash128::create(cc::span<quadric_primitive const>(_primitives).as_bytes(), impl::quadric_hash_seed);
        _hash_dirty = false;
    }
    return _hash;
}

void sv::quadric_set::add_arrow(tg::segment3f const& s, arrow_style const& style)
{
    for (auto const& p : arrow_primitives(s, style))
        add(p);
}

void sv::quadric_set::add_capsule(tg::segment3f const& s, float radius)
{
    for (auto const& p : capsule_primitives(s, radius))
        add(p);
}

isize sv::quadric_set::add_vertices(cc::span<tg::pos3f const> points, vertex_style const& style)
{
    for (auto const& p : points)
        add_sphere(tg::sphere3f(p, style.radius));
    return points.size();
}

isize sv::quadric_set::add_edges(cc::span<tg::segment3f const> edges,
                                 cc::span<tg::pos3f const> vertices,
                                 edge_style const& style)
{
    auto const before = primitive_count();
    auto const joined = style.vertices && !vertices.empty();
    for (auto const& e : edges)
        add_line(e, {.radius = style.radius, .ends = joined ? line_ends::open : style.ends});
    if (joined)
        add_vertices(vertices, {.radius = style.joint_radius()});
    return primitive_count() - before;
}

void sv::quadric_set::clear()
{
    _primitives.clear();
    _hash = {};
    _hash_dirty = false; // an empty set's key is the default, which is what a never-filled one already reports
    _bounds = {};
}
