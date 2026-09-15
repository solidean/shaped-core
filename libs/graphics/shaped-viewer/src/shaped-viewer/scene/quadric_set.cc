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
    // Folding the primitive's own digest in keeps this O(1) and keeps the result order-sensitive, which it must be:
    // primitive order is what `PrimitiveIndex()` reads, so two sets holding the same primitives in a different order are
    // different resources and must not share a cache entry.
    auto const digest = cc::hash128::create(cc::span<quadric_primitive const>(&p, 1).as_bytes(), impl::quadric_hash_seed);
    _hash = impl::combine_digests(_hash, digest);

    _bounds = _bounds.has_value() ? united(_bounds.value(), p.bounds) : p.bounds;

    _primitives.push_back(p);
}

void sv::quadric_set::add_capsule(tg::segment3f const& s, float radius)
{
    add(quadric_primitive::create_cylinder(s, radius));
    add(quadric_primitive::create_sphere(tg::sphere3f(s.pos0, radius)));
    add(quadric_primitive::create_sphere(tg::sphere3f(s.pos1, radius)));
}

void sv::quadric_set::clear()
{
    _primitives.clear();
    _hash = {};
    _bounds = {};
}
