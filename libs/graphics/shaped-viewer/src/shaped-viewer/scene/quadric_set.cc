#include "quadric_set.hh"

#include <clean-core/common/utility.hh> // cc::min, cc::max
#include <shaped-viewer/impl/content_hash.hh>

namespace
{
tg::aabb3f united(tg::aabb3f const& a, tg::aabb3f const& b)
{
    auto const lo = tg::pos3f(cc::min(a.min[0], b.min[0]), cc::min(a.min[1], b.min[1]), cc::min(a.min[2], b.min[2]));
    auto const hi = tg::pos3f(cc::max(a.max[0], b.max[0]), cc::max(a.max[1], b.max[1]), cc::max(a.max[2], b.max[2]));
    return tg::aabb3f(lo, hi);
}
} // namespace

void sv::quadric_set::add(quadric_primitive const& p)
{
    // The hash is deferred rather than folded: one streaming pass over the span beats 2N short-input calls by about
    // five times for the same invariant, and nothing here asks for the key until the set is placed.
    _hash_dirty = true;

    // The bounds fold stays, because a union of boxes is not a byte range and has no bulk form to defer to.
    _bounds = _bounds.has_value() ? united(_bounds.value(), p.bounds) : p.bounds;

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

void sv::quadric_set::clear()
{
    _primitives.clear();
    _hash = {};
    _hash_dirty = false; // an empty set's key is the default, which is what a never-filled one already reports
    _bounds = {};
}
