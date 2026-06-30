#include <nexus/test.hh>
#include <typed-geometry/geometry/geometry.hh>

// object_traits is a pure compile-time seam; assert the declared facts for representative
// instantiations through the tg::traits::* helpers.

namespace
{
// aabb: full-dimensional and finite.
static_assert(tg::traits::intrinsic_dim<tg::aabb3f> == 3);
static_assert(tg::traits::ambient_dim<tg::aabb3f> == 3);
static_assert(tg::traits::is_finite<tg::aabb3f>);
static_assert(tg::traits::intrinsic_dim<tg::aabb2f> == 2);

// triangle: a 2D surface patch, finite, regardless of the ambient space.
static_assert(tg::traits::intrinsic_dim<tg::triangle3f> == 2);
static_assert(tg::traits::ambient_dim<tg::triangle3f> == 3);
static_assert(tg::traits::is_finite<tg::triangle3f>);
static_assert(tg::traits::intrinsic_dim<tg::triangle2f> == 2);
static_assert(tg::traits::ambient_dim<tg::triangle2f> == 2);

// segment: 1D and finite.
static_assert(tg::traits::intrinsic_dim<tg::segment3f> == 1);
static_assert(tg::traits::ambient_dim<tg::segment3f> == 3);
static_assert(tg::traits::is_finite<tg::segment3f>);

// ray / line: 1D and infinite.
static_assert(tg::traits::intrinsic_dim<tg::ray2f> == 1);
static_assert(tg::traits::ambient_dim<tg::ray2f> == 2);
static_assert(!tg::traits::is_finite<tg::ray2f>);
static_assert(tg::traits::intrinsic_dim<tg::line3f> == 1);
static_assert(!tg::traits::is_finite<tg::line3f>);

// plane: codimension 1 and infinite (a line in 2D, a plane in 3D).
static_assert(tg::traits::intrinsic_dim<tg::plane3f> == 2);
static_assert(tg::traits::ambient_dim<tg::plane3f> == 3);
static_assert(!tg::traits::is_finite<tg::plane3f>);
static_assert(tg::traits::intrinsic_dim<tg::plane2f> == 1);
} // namespace

TEST("tg object_traits - point-set facts")
{
    // The invariants above are static_asserts; this runtime check keeps the file a discoverable
    // test and documents the intrinsic <= ambient relationship.
    CHECK(tg::traits::intrinsic_dim<tg::triangle3f> <= tg::traits::ambient_dim<tg::triangle3f>);
    CHECK(tg::traits::intrinsic_dim<tg::plane3f> == tg::traits::ambient_dim<tg::plane3f> - 1);
}
