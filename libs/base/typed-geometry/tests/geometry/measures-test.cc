#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/primitives.hh>

// The unary members: measures by intrinsic dimension, bounds, decomposition and the parameter maps.
// A solid's area() is its boundary's area; a boundary answers only its own measure.

namespace
{
template <class T>
concept has_volume = requires(T const& t) { t.volume(); };
template <class T>
concept has_length = requires(T const& t) { t.length(); };

static_assert(has_volume<tg::sphere3f>);
static_assert(!has_volume<tg::sphere3f_surface>, "a surface has no volume; ask its solid");
static_assert(!has_volume<tg::sphere2f>, "a disk is 2D");
static_assert(has_length<tg::sphere2f_boundary>, "a circle's measure is its length");
static_assert(!has_length<tg::sphere2f>, "a disk's outline is perimeter(), not length()");

static_assert(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(3, 2)).area() == 6, "exact on ints");
static_assert(tg::triangle2i(tg::pos2i(0, 0), tg::pos2i(4, 0), tg::pos2i(0, 4)).area() == 8);
} // namespace

TEST("tg measures - by intrinsic dimension")
{
    auto const pi = tg::pi<float>;

    CHECK(tgtest::approx(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 3, 4)).length(), 5.0f));

    auto const tri = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(3, 0, 0), tg::pos3f(0, 4, 0));
    CHECK(tgtest::approx(tri.area(), 6.0f));
    CHECK(tgtest::approx(tri.perimeter(), 12.0f));

    auto const box = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 2, 3));
    CHECK(tgtest::approx(box.volume(), 6.0f));
    CHECK(tgtest::approx(box.area(), 22.0f));
    CHECK(tgtest::approx(box.boundary().area(), 22.0f));
    CHECK(tgtest::approx(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 2)).perimeter(), 6.0f));
    CHECK(tgtest::approx(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 2)).boundary().length(), 6.0f));

    auto const s = tg::sphere3f(tg::pos3f(1, 1, 1), 2.0f);
    CHECK(tgtest::approx(s.volume(), 32.0f / 3.0f * pi, 1e-3f));
    CHECK(tgtest::approx(s.area(), 16.0f * pi, 1e-3f));
    CHECK(tgtest::approx(s.boundary().area(), 16.0f * pi, 1e-3f));
    CHECK(tgtest::approx(tg::sphere2f(tg::pos2f(0, 0), 2.0f).area(), 4.0f * pi, 1e-3f));
    CHECK(tgtest::approx(tg::sphere2f(tg::pos2f(0, 0), 2.0f).boundary().length(), 4.0f * pi, 1e-3f));
    CHECK(tgtest::approx(tg::disk3f(tg::pos3f(0, 0, 0), 1.0f, tg::vec3f(0, 0, 1)).perimeter(), 2.0f * pi, 1e-4f));

    auto const e = tg::ellipsoid3f(tg::pos3f(0, 0, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 2, 0), tg::vec3f(0, 0, 3));
    CHECK(tgtest::approx(e.volume(), 8.0f * pi, 1e-3f));

    auto const h = tg::mat3f::make_from_cols(tg::vec3f(1, 0, 0), tg::vec3f(0, 2, 0), tg::vec3f(0, 0, 3));
    auto const b = tg::box3f(tg::pos3f(0, 0, 0), h);
    CHECK(tgtest::approx(b.volume(), 48.0f));
    CHECK(tgtest::approx(b.area(), 88.0f));
}

TEST("tg measures - centroid and bounds")
{
    auto const tri = tg::triangle2f(tg::pos2f(0, 0), tg::pos2f(3, 0), tg::pos2f(0, 3));
    CHECK(tgtest::approx(tri.centroid(), tg::pos2f(1, 1)));
    CHECK(tri.bounds() == tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(3, 3)));

    CHECK(tg::sphere3f(tg::pos3f(1, 1, 1), 2.0f).bounds() == tg::aabb3f(tg::pos3f(-1, -1, -1), tg::pos3f(3, 3, 3)));

    // a disk lying flat in z reaches its radius along x and y, and nothing along z
    auto const d = tg::disk3f(tg::pos3f(0, 0, 0), 2.0f, tg::vec3f(0, 0, 1)).bounds();
    CHECK(tgtest::approx(d.max, tg::pos3f(2, 2, 0)));

    // a box rotated 45 degrees reaches sqrt(2) along x and y
    auto const t = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(45)));
    auto const b = tg::aabb3f(tg::pos3f(-1, -1, -1), tg::pos3f(1, 1, 1)).transformed(t).bounds();
    CHECK(tgtest::approx(b.max, tg::pos3f(1.41421f, 1.41421f, 1)));
}

TEST("tg measures - decomposition")
{
    auto const a = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 2, 3));
    auto const v = a.vertices();
    CHECK(v[0] == a.min);
    CHECK(v[7] == a.max);
    CHECK(v[1] == tg::pos3f(1, 0, 0));

    auto const edges = a.edges();
    static_assert(sizeof(edges) == sizeof(tg::segment3f) * 12);
    auto total = 0.0f;
    for (auto i = 0; i < 12; ++i)
        total += edges[i].length();
    CHECK(tgtest::approx(total, 4.0f * (1 + 2 + 3)));

    auto const tri = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0));
    CHECK(tgtest::approx(tri.normal(), tg::vec3f(0, 0, 1)));
    CHECK(tgtest::approx(tri.plane().dist, 0.0f));
    CHECK(tri.edges()[1] == tg::segment3f(tri.pos1, tri.pos2));

    CHECK(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(2, 0, 0)).unbounded().dir == tg::vec3f(2, 0, 0));
}

TEST("tg measures - parameters invert evaluation")
{
    auto const s = tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 0, 0));
    CHECK(s.at(0.25f) == tg::pos3f(1, 0, 0));
    CHECK(tgtest::approx(s.parameter_of(tg::pos3f(1, 5, 0)), 0.25f));
    CHECK(s.parameter_of(tg::pos3f(-3, 0, 0)) == 0.0f);

    auto const r = tg::ray3f(tg::pos3f(0, 0, 0), tg::vec3f(0, 0, 2));
    CHECK(r.at(1.5f) == tg::pos3f(0, 0, 3));
    CHECK(r.parameter_of(tg::pos3f(0, 0, -5)) == 0.0f);

    auto const tri = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 0, 0), tg::pos3f(0, 4, 0));
    auto const b = tri.parameter_of(tg::pos3f(1, 2, 7));
    CHECK(tgtest::approx(tri.at(b), tg::pos3f(1, 2, 0)));
    CHECK(tgtest::approx(b.data[0], 0.25f));
    CHECK(tri.parameter_of(tg::pos3f(-1, 0, 0)).data[1] < 0.0f);

    auto const a = tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(4, 2));
    CHECK(a.at(tg::comp2f(0.5f, 0.5f)) == tg::pos2f(2, 1));
    CHECK(tgtest::approx(a.parameter_of(tg::pos2f(1, 1)).data[0], 0.25f));

    auto const h = tg::mat2f::make_from_cols(tg::vec2f(2, 0), tg::vec2f(0, 1));
    auto const bx = tg::box2f(tg::pos2f(1, 1), h);
    CHECK(bx.at(tg::comp2f(1, -1)) == tg::pos2f(3, 0));
    CHECK(tgtest::approx(bx.parameter_of(tg::pos2f(3, 0)).data[1], -1.0f));
}
