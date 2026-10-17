#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/geometry/query/query.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

#include <type_traits>

using namespace cc::primitive_defines;

static_assert(std::is_trivially_copyable_v<tg::aabb3f>, "aabb should be trivially copyable");

TEST("tg aabb - construction")
{
    SECTION("default is the empty box")
    {
        tg::aabb3f b;
        CHECK(b.is_empty());
        CHECK(b == tg::aabb3f::empty);
        CHECK(b.min
              == tg::pos3f(tg::traits::highest<f32>(), tg::traits::highest<f32>(),
                           tg::traits::highest<f32>()));
        CHECK(b.max
              == tg::pos3f(tg::traits::lowest<f32>(), tg::traits::lowest<f32>(), tg::traits::lowest<f32>()));
    }

    SECTION("explicit ctor stores min/max")
    {
        auto const lo = tg::pos3f(0, 0, 0);
        auto const hi = tg::pos3f(1, 2, 3);
        tg::aabb3f const b(lo, hi);
        CHECK(b.min == lo);
        CHECK(b.max == hi);
        CHECK(!b.is_empty());
    }

    SECTION("a flat or degenerate box is not empty")
    {
        CHECK(!tg::aabb3f(tg::pos3f(1, 2, 3), tg::pos3f(1, 2, 3)).is_empty());
        CHECK(!tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 1)).is_empty());
    }

    SECTION("every scalar has an empty box")
    {
        CHECK(tg::aabb2i().is_empty());
        CHECK((tg::aabb<2, u8>().is_empty()));
        CHECK((tg::aabb<3, tg::fi128>().is_empty()));
        CHECK((tg::aabb<3, tg::f16>().is_empty()));
    }
}

TEST("tg aabb - equality")
{
    auto const b = tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(1, 1));
    CHECK(b == tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(1, 1)));
    CHECK(b != tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(2, 1)));
}

TEST("tg aabb - the empty box is the identity of union, and holds nothing")
{
    auto const unit = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1));

    SECTION("union")
    {
        CHECK(tg::aabb_of(tg::aabb3f::empty, unit) == unit);
        CHECK(tg::aabb_of(unit, tg::aabb3f::empty) == unit);
        CHECK(tg::aabb_of(tg::aabb3f::empty, tg::aabb3f::empty).is_empty());
    }
    SECTION("containment and intersection")
    {
        CHECK(!tg::aabb3f::empty.contains(tg::pos3f(0, 0, 0)));
        CHECK(!tg::aabb3f::empty.intersects(unit));
        CHECK(!unit.intersects(tg::aabb3f::empty));
        CHECK(unit.contains(tg::aabb3f::empty));
        CHECK(tg::aabb3f::empty.contains(tg::aabb3f::empty));
        CHECK(!tg::aabb3f::empty.contains(unit));
        CHECK(!unit.intersection_with(tg::aabb3f::empty).has_value());
    }
    SECTION("a transform keeps it empty, a negative scale included")
    {
        CHECK(tg::aabb3f::empty.transformed(tg::scaling_translation_transform3f::make_translation(tg::vec3f(1, 2, 3)))
                  .is_empty());
        CHECK(tg::aabb3f::empty.transformed(tg::signed_scaling_transform3f::make_scaling(tg::vec3f(-1, 2, -3))).is_empty());
    }
    SECTION("the measures and readings refuse it")
    {
        CHECK_ASSERTS(tg::aabb3f::empty.volume());
        CHECK_ASSERTS(tg::aabb3f::empty.centroid());
        CHECK_ASSERTS(tg::aabb3f::empty.vertices());
        CHECK_ASSERTS(tg::aabb3f::empty.distance_to(tg::pos3f(0, 0, 0)));
    }
}

TEST("tg aabb - aabb_of accumulates points, objects and ranges")
{
    auto const p = tg::pos3f(1, -2, 0);
    auto const tri = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 0, 0), tg::pos3f(0, 1, 5));
    auto const ball = tg::sphere3f(tg::pos3f(10, 10, 10), 1);
    auto points = cc::vector<tg::pos3f>();
    points.push_back(tg::pos3f(-3, 0, 0));
    points.push_back(tg::pos3f(0, 7, 0));

    SECTION("a single point is a degenerate box")
    {
        CHECK(tg::aabb_of(p) == tg::aabb3f(p, p));
    }
    SECTION("everything at once")
    {
        auto const b = tg::aabb_of(p, tri, points, ball);
        CHECK(b == tg::aabb3f(tg::pos3f(-3, -2, 0), tg::pos3f(11, 11, 11)));
    }
    SECTION("a range of objects")
    {
        tg::segment3f const segs[] = {tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1)),
                                      tg::segment3f(tg::pos3f(-1, 5, 0), tg::pos3f(0, 0, 2))};
        CHECK(tg::aabb_of(segs) == tg::aabb3f(tg::pos3f(-1, 0, 0), tg::pos3f(1, 5, 2)));
    }
    SECTION("an empty range is the empty box, and changes nothing next to others")
    {
        auto const none = cc::vector<tg::pos3f>();
        CHECK(tg::aabb_of(none).is_empty());
        CHECK(tg::aabb_of(none, p) == tg::aabb3f(p, p));
    }
    SECTION("the static spelling names the box type, and converts the scalar")
    {
        CHECK(tg::aabb3f::of(points) == tg::aabb_of(points));
        CHECK(tg::aabb3d::of(tg::pos3f(1, 2, 3), tg::pos3d(-1, 0, 0))
              == tg::aabb3d(tg::pos3d(-1, 0, 0), tg::pos3d(1, 2, 3)));
        CHECK(tg::aabb3f::of().is_empty());
    }
    SECTION("an exact scalar works too")
    {
        CHECK(tg::aabb_of(tg::pos2i(3, -1), tg::pos2i(-2, 4)) == tg::aabb2i(tg::pos2i(-2, -1), tg::pos2i(3, 4)));
    }
}
