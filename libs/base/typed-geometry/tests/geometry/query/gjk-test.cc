#include "../../approx.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <nexus/tests/thorough.hh>
#include <typed-geometry/geometry/query/query.hh>

// The GJK floor against closed forms on random convex pairs, and EPA's depth on overlaps whose depth is known.
// The pairs here have no closed-form kernel, so every query below reaches GJK through the ladder.
// Every pair of 3D types with a support is also checked against the certificate its supports give, closed form or not.

namespace
{
static_assert(tg::has_distance_sqr_to<tg::sphere3d, tg::aabb3d>);
static_assert(tg::has_intersects<tg::triangle3d, tg::segment3d>);
static_assert(!tg::has_distance_sqr_to<tg::aabb3i, tg::triangle3i>, "GJK iterates to a tolerance, so ints are refused");
static_assert(tg::has_separation_from<tg::sphere3d, tg::aabb3d>);
static_assert(!tg::has_separation_from<tg::triangle3d, tg::aabb3d>, "EPA needs two full-dimensional solids");

tg::pos3d random_pos(cc::random& rng, double extent)
{
    return tg::pos3d(rng.uniform(-extent, extent), rng.uniform(-extent, extent), rng.uniform(-extent, extent));
}

tg::aabb3d random_aabb(cc::random& rng)
{
    auto const c = random_pos(rng, 4.0);
    auto const h = tg::vec3d(rng.uniform(0.1, 2.0), rng.uniform(0.1, 2.0), rng.uniform(0.1, 2.0));
    return tg::aabb3d(c - h, c + h);
}

/// per-axis gap between two boxes, the exact distance
double aabb_distance(tg::aabb3d const& a, tg::aabb3d const& b)
{
    auto d2 = 0.0;
    for (auto i = 0; i < 3; ++i)
    {
        auto const gap = cc::max(a.min.data[i] - b.max.data[i], b.min.data[i] - a.max.data[i]);
        if (gap > 0)
            d2 += gap * gap;
    }
    return tg::sqrt(d2);
}

// --- random instances of every 3D type with a support, for the certificate test

tg::vec3d random_dir(cc::random& rng)
{
    while (true)
    {
        auto const v = random_pos(rng, 1.0) - tg::pos3d();
        auto const l2 = v.length_sqr();
        if (l2 > 0.01 && l2 <= 1.0)
            return v / tg::sqrt(l2);
    }
}

/// a random orthonormal frame, (u, w, n)
tg::mat3d random_rotation(cc::random& rng)
{
    auto const n = random_dir(rng);
    auto const [u, w] = tg::orthonormal_basis(n);
    return tg::mat3d::make_from_cols(u, w, n);
}

template <class T>
struct tag
{
};

tg::sphere3d make(cc::random& rng, tag<tg::sphere3d>)
{
    return tg::sphere3d(random_pos(rng, 3.0), rng.uniform(0.1, 2.0));
}
tg::aabb3d make(cc::random& rng, tag<tg::aabb3d>)
{
    return random_aabb(rng);
}
tg::box3d make(cc::random& rng, tag<tg::box3d>)
{
    auto const r = random_rotation(rng);
    auto const h = tg::mat3d::make_from_cols(r.cols[0] * rng.uniform(0.05, 1.5), r.cols[1] * rng.uniform(0.05, 1.5),
                                             r.cols[2] * rng.uniform(0.05, 1.5));
    return tg::box3d(random_pos(rng, 3.0), h);
}
tg::capsule3d make(cc::random& rng, tag<tg::capsule3d>)
{
    auto const c = random_pos(rng, 3.0);
    auto const h = random_dir(rng) * rng.uniform(0.0, 2.0);
    return tg::capsule3d(tg::segment3d(c - h, c + h), rng.uniform(0.05, 1.5));
}
tg::cylinder3d make(cc::random& rng, tag<tg::cylinder3d>)
{
    auto const c = random_pos(rng, 3.0);
    auto const h = random_dir(rng) * rng.uniform(0.05, 2.0);
    return tg::cylinder3d(tg::segment3d(c - h, c + h), rng.uniform(0.05, 1.5));
}
tg::cone3d make(cc::random& rng, tag<tg::cone3d>)
{
    return tg::cone3d(random_pos(rng, 3.0), random_dir(rng) * rng.uniform(0.1, 3.0), rng.uniform(0.05, 1.5));
}
tg::hemisphere3d make(cc::random& rng, tag<tg::hemisphere3d>)
{
    return tg::hemisphere3d(random_pos(rng, 3.0), rng.uniform(0.1, 2.0), random_dir(rng));
}
tg::ellipsoid3d make(cc::random& rng, tag<tg::ellipsoid3d>)
{
    auto const r = random_rotation(rng);
    return tg::ellipsoid3d(random_pos(rng, 3.0), r.cols[0] * rng.uniform(0.05, 2.0), r.cols[1] * rng.uniform(0.05, 2.0),
                           r.cols[2] * rng.uniform(0.05, 2.0));
}
tg::tetrahedron3d make(cc::random& rng, tag<tg::tetrahedron3d>)
{
    auto const c = random_pos(rng, 3.0) - tg::pos3d();
    return tg::tetrahedron3d(random_pos(rng, 1.5) + c, random_pos(rng, 1.5) + c, random_pos(rng, 1.5) + c,
                             random_pos(rng, 1.5) + c);
}
tg::segment3d make(cc::random& rng, tag<tg::segment3d>)
{
    auto const c = random_pos(rng, 3.0) - tg::pos3d();
    return tg::segment3d(random_pos(rng, 2.0) + c, random_pos(rng, 2.0) + c);
}
tg::triangle3d make(cc::random& rng, tag<tg::triangle3d>)
{
    auto const c = random_pos(rng, 3.0) - tg::pos3d();
    return tg::triangle3d(random_pos(rng, 2.0) + c, random_pos(rng, 2.0) + c, random_pos(rng, 2.0) + c);
}
/// a perspective frustum with a far plane, rigidly placed: its apex, a frame, two half-angle slopes, near and far
tg::frustum3d make(cc::random& rng, tag<tg::frustum3d>)
{
    auto const apex = random_pos(rng, 3.0);
    auto const r = random_rotation(rng);
    auto const sx = rng.uniform(0.2, 1.5);
    auto const sy = rng.uniform(0.2, 1.5);
    auto const near_ = rng.uniform(0.1, 1.0);
    auto const far_ = near_ + rng.uniform(0.1, 2.5);
    // a plane given in the frustum's own frame, moved to the world
    auto const place = [&](tg::vec3d const& n, double dist)
    {
        auto const wn = r * n.normalized();
        return tg::plane3d(wn, dist / n.length() + tg::dot(wn, apex - tg::pos3d()));
    };
    return tg::frustum3d(place(tg::vec3d(-1, 0, -sx), 0), place(tg::vec3d(1, 0, -sx), 0),
                         place(tg::vec3d(0, -1, -sy), 0), place(tg::vec3d(0, 1, -sy), 0),
                         place(tg::vec3d(0, 0, -1), -near_), place(tg::vec3d(0, 0, 1), far_));
}

// --- each instance as the constructor call that rebuilds it, so a failure can be pinned verbatim

cc::string str(tg::pos3d const& p)
{
    return cc::format("tg::pos3d({}, {}, {})", p.data[0], p.data[1], p.data[2]);
}
cc::string str(tg::vec3d const& v)
{
    return cc::format("tg::vec3d({}, {}, {})", v.data[0], v.data[1], v.data[2]);
}
cc::string str(tg::segment3d const& s)
{
    return cc::format("tg::segment3d({}, {})", str(s.pos0), str(s.pos1));
}
cc::string str(tg::sphere3d const& s)
{
    return cc::format("tg::sphere3d({}, {})", str(s.center), s.radius);
}
cc::string str(tg::aabb3d const& b)
{
    return cc::format("tg::aabb3d({}, {})", str(b.min), str(b.max));
}
cc::string str(tg::box3d const& b)
{
    auto const& h = b.half_extents.cols;
    return cc::format("tg::box3d({}, tg::mat3d::make_from_cols({}, {}, {}))", str(b.center), str(h[0]), str(h[1]),
                      str(h[2]));
}
cc::string str(tg::capsule3d const& c)
{
    return cc::format("tg::capsule3d({}, {})", str(c.axis), c.radius);
}
cc::string str(tg::cylinder3d const& c)
{
    return cc::format("tg::cylinder3d({}, {})", str(c.axis), c.radius);
}
cc::string str(tg::cone3d const& c)
{
    return cc::format("tg::cone3d({}, {}, {})", str(c.apex), str(c.axis), c.radius);
}
cc::string str(tg::hemisphere3d const& h)
{
    return cc::format("tg::hemisphere3d({}, {}, {})", str(h.center), h.radius, str(h.normal));
}
cc::string str(tg::ellipsoid3d const& e)
{
    return cc::format("tg::ellipsoid3d({}, {}, {}, {})", str(e.center), str(e.semi_axes[0]), str(e.semi_axes[1]),
                      str(e.semi_axes[2]));
}
cc::string str(tg::tetrahedron3d const& t)
{
    return cc::format("tg::tetrahedron3d({}, {}, {}, {})", str(t.pos0), str(t.pos1), str(t.pos2), str(t.pos3));
}
cc::string str(tg::triangle3d const& t)
{
    return cc::format("tg::triangle3d({}, {}, {})", str(t.pos0), str(t.pos1), str(t.pos2));
}
cc::string str(tg::frustum3d const& f)
{
    auto r = cc::string("tg::frustum3d(");
    for (auto i = 0; i < 6; ++i)
        r.appendf("{}tg::plane3d({}, {})", i == 0 ? "" : ", ", str(f.planes[i].normal), f.planes[i].dist);
    r += ")";
    return r;
}

/// the directions a separating axis among a polytope's own faces would be found along
using axis_list = cc::fixed_vector<tg::vec3d, 32>;
template <class Obj>
void add_face_normals(Obj const&, axis_list&)
{
}
void add_face_normals(tg::box3d const& b, axis_list& axes)
{
    auto const& h = b.half_extents.cols;
    axes.push_back(tg::dual(tg::cross(h[0], h[1])));
    axes.push_back(tg::dual(tg::cross(h[1], h[2])));
    axes.push_back(tg::dual(tg::cross(h[2], h[0])));
}
void add_face_normals(tg::tetrahedron3d const& t, axis_list& axes)
{
    for (auto const& f : t.faces())
        axes.push_back(tg::dual(tg::cross(f.pos1 - f.pos0, f.pos2 - f.pos0)));
}
void add_face_normals(tg::triangle3d const& t, axis_list& axes)
{
    axes.push_back(tg::dual(tg::cross(t.pos1 - t.pos0, t.pos2 - t.pos0)));
}
void add_face_normals(tg::frustum3d const& f, axis_list& axes)
{
    for (auto const& pl : f.planes)
        axes.push_back(pl.normal);
}

/// the largest coordinate magnitude of an object's bounds, which every tolerance below is relative to
template <class Obj>
double extent_of(Obj const& o)
{
    auto const b = o.bounds();
    auto r = 1.0;
    for (auto i = 0; i < 3; ++i)
        r = cc::max(r, cc::max(tg::abs(b.min.data[i]), tg::abs(b.max.data[i])));
    return r;
}

/// how far a point lies outside an object, or 0 where the pair has no distance to a point
template <class Obj>
double outside_by(Obj const& o, tg::pos3d const& p)
{
    if constexpr (tg::has_distance_sqr_to<Obj, tg::pos3d>)
        return tg::sqrt(tg::impl::distance_sqr_to(o, p));
    else
        return 0.0;
}

template <class Obj>
tg::vec3d support(Obj const& o, tg::vec3d const& dir)
{
    return tg::impl::support_op<Obj>::apply(o, dir) - tg::pos3d();
}

/// relative to the larger extent of the pair; the certified pairs stay below 4e-11 over a thorough run
constexpr auto certificate_tolerance = 1e-9;

/// A cone's generators all meet at its apex, so GJK's last faces against its slant are slivers that lose ~1e-8 to rounding.
template <class T>
constexpr bool is_cone = false;
template <>
constexpr bool is_cone<tg::cone3d> = true;
template <class A, class B>
constexpr auto pair_tolerance = is_cone<A> || is_cone<B> ? 1e-7 : certificate_tolerance;

/// How far GJK's answer on a pair is from what the supports alone certify, relative to the pair's extent.
///
/// Apart, the supports along the line between the closest points must reach exactly those points, which lie in
/// their objects; the supports' gap along that line is then a separating slab as wide as the distance.
/// Overlapping, no candidate axis — between the centroids, the coordinate axes, a polytope's face normals — may
/// separate the two, since any one that does is a separating plane.
/// Apart by less than the tolerance is touching, where the direction between the points is noise.
struct certificate
{
    bool overlapping = false;
    double residual = 0;
};

template <class A, class B>
certificate certify(A const& a, B const& b)
{
    auto const g = tg::impl::gjk(a, b);
    auto const scale = cc::max(extent_of(a), extent_of(b));

    if (!g.overlapping)
    {
        auto const diff = g.on_b - g.on_a;
        auto const d = diff.length();
        auto residual = cc::max(outside_by(a, g.on_a), outside_by(b, g.on_b));
        if (d > certificate_tolerance * scale)
        {
            auto const n = diff / d;
            auto const sa = tg::dot(support(a, n), n);
            auto const sb = tg::dot(support(b, -n), n);
            residual = cc::max(residual, tg::abs(sa - tg::dot(g.on_a - tg::pos3d(), n)));
            residual = cc::max(residual, tg::abs(sb - tg::dot(g.on_b - tg::pos3d(), n)));
            residual = cc::max(residual, tg::abs((sb - sa) - d));
        }
        return {.overlapping = false, .residual = residual / scale};
    }

    auto axes = axis_list();
    axes.push_back(b.centroid() - a.centroid());
    axes.push_back(tg::vec3d(1, 0, 0));
    axes.push_back(tg::vec3d(0, 1, 0));
    axes.push_back(tg::vec3d(0, 0, 1));
    add_face_normals(a, axes);
    add_face_normals(b, axes);

    auto separation = 0.0;
    for (auto const& axis : axes)
    {
        auto const n = axis.normalized();
        if (n == tg::vec3d())
            continue;
        // B beyond A along n, or A beyond B
        separation = cc::max(separation, tg::dot(support(b, -n), n) - tg::dot(support(a, n), n));
        separation = cc::max(separation, tg::dot(support(a, -n), n) - tg::dot(support(b, n), n));
    }
    return {.overlapping = true, .residual = separation / scale};
}

template <class A, class B>
double certificate_residual(A const& a, B const& b)
{
    return certify(a, b).residual;
}

/// Random instances of one pair, in both argument orders since GJK is not symmetric in them.
/// Stops at the first violation, printing the pair as the constructor calls that rebuild it.
template <class A, class B>
void certify_pair(int iterations)
{
    constexpr auto tolerance = pair_tolerance<A, B>;
    auto rng = cc::random(101);
    auto worst = 0.0;
    for (auto i = 0; i < iterations; ++i)
    {
        auto const a = make(rng, tag<A>());
        auto const b = make(rng, tag<B>());
        auto const c = i % 2 == 0 ? certify(a, b) : certify(b, a);
        worst = cc::max(worst, c.residual);
        if (c.residual > tolerance)
        {
            CHECK(c.residual <= tolerance)
                .dump("first", i % 2 == 0 ? str(a) : str(b))
                .dump("second", i % 2 == 0 ? str(b) : str(a))
                .dump("gjk says", cc::string(c.overlapping ? "overlapping" : "apart"));
            return;
        }
    }
    CHECK(worst <= tolerance);
}

/// Every unordered pair of the types, itself included.
template <class A, class... Rest>
void certify_all(int iterations)
{
    certify_pair<A, A>(iterations);
    (certify_pair<A, Rest>(iterations), ...);
    if constexpr (sizeof...(Rest) > 0)
        certify_all<Rest...>(iterations);
}
} // namespace

TEST("tg gjk - distances match the closed forms")
{
    auto rng = cc::random(7);

    for (auto i = 0; i < 300; ++i)
    {
        auto const a = random_aabb(rng);
        auto const b = random_aabb(rng);
        auto const s = tg::sphere3d(random_pos(rng, 4.0), rng.uniform(0.1, 2.0));
        auto const t = tg::sphere3d(random_pos(rng, 4.0), rng.uniform(0.1, 2.0));

        auto const ab = aabb_distance(a, b);
        CHECK(tgtest::approx(a.distance_to(b), ab, 1e-6));
        CHECK(a.intersects(b) == (ab == 0.0));

        auto const st = cc::max(0.0, (s.center - t.center).length() - s.radius - t.radius);
        CHECK(tgtest::approx(s.distance_to(t), st, 1e-6));

        // sphere to box through the projection of the center, which is exact
        auto const sa = cc::max(0.0, s.center.distance_to(a) - s.radius);
        CHECK(tgtest::approx(s.distance_to(a), sa, 1e-6));
    }
}

TEST("tg gjk - closest points lie on their objects and realize the distance")
{
    auto rng = cc::random(11);

    for (auto i = 0; i < 200; ++i)
    {
        auto const b = random_aabb(rng);
        auto const tri = tg::triangle3d(random_pos(rng, 5.0), random_pos(rng, 5.0), random_pos(rng, 5.0));

        auto const [on_tri, on_b] = tri.closest_points_to(b);
        auto const d = tri.distance_to(b);

        CHECK(tgtest::approx(on_tri.distance_to(on_b), d, 1e-6));
        CHECK(on_tri.distance_to(tri) < 1e-6);
        CHECK(b.contains(on_b.project_to(b)));
        CHECK(on_b.distance_to(b) < 1e-9);

        // dense sampling of the triangle only ever finds a farther point
        auto best = 1e30;
        for (auto u = 0; u <= 40; ++u)
            for (auto v = 0; u + v <= 40; ++v)
            {
                auto const p = tri.pos0 + (tri.pos1 - tri.pos0) * (u / 40.0) + (tri.pos2 - tri.pos0) * (v / 40.0);
                best = cc::min(best, p.distance_to(b));
            }
        CHECK(d <= best + 1e-9);
        CHECK(best - d < 0.5);
    }
}

TEST("tg epa - depth and direction of known overlaps")
{
    SECTION("two balls: the depth is the overlap of their radii, along the line of centers")
    {
        auto const a = tg::sphere3d(tg::pos3d(0, 0, 0), 1.0);
        auto const b = tg::sphere3d(tg::pos3d(1.5, 0, 0), 1.0);
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.5, 1e-3));
        CHECK(tgtest::approx(s.value().normal, tg::vec3d(1, 0, 0), 1e-3));
    }

    SECTION("two boxes: the axis of least overlap")
    {
        auto const a = tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(2, 2, 2));
        auto const b = tg::aabb3d(tg::pos3d(1.75, 0.5, 0.5), tg::pos3d(3, 1.5, 1.5));
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.25, 1e-6));
        CHECK(tgtest::approx(s.value().normal, tg::vec3d(1, 0, 0), 1e-6));
    }

    SECTION("moving b by the separation leaves the solids touching, not overlapping")
    {
        auto const a = tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(2, 2, 2));
        auto const b = tg::sphere3d(tg::pos3d(1, 1, 2.5), 1.0);
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.5, 1e-3));
        auto const moved = tg::sphere3d(b.center + s.value().normal * s.value().depth, b.radius);
        CHECK(a.distance_to(moved) < 1e-3);
    }

    SECTION("in 2D")
    {
        auto const a = tg::sphere2d(tg::pos2d(0, 0), 1.0);
        auto const b = tg::aabb2d(tg::pos2d(0.75, -1), tg::pos2d(3, 1));
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.25, 1e-3));
        CHECK(tgtest::approx(s.value().normal, tg::vec2d(1, 0), 1e-3));
    }

    SECTION("apart: no separation is needed")
    {
        auto const a = tg::sphere3d(tg::pos3d(0, 0, 0), 1.0);
        auto const b = tg::sphere3d(tg::pos3d(3, 0, 0), 1.0);
        CHECK(!a.separation_from(b).has_value());
    }
}

TEST("tg gjk - every pair with a support agrees with what the supports certify")
{
    // Most pairs have no closed form, so GJK is checked against its own certificate.
    certify_all<tg::sphere3d, tg::aabb3d, tg::box3d, tg::capsule3d, tg::cylinder3d, tg::cone3d, tg::hemisphere3d,
                tg::ellipsoid3d, tg::tetrahedron3d, tg::segment3d, tg::triangle3d, tg::frustum3d>(
        nx::is_thorough() ? 20000 : 400);
}

TEST("tg gjk - a capped support along its axis stays in the solid")
{
    // A direction (anti)parallel to the axis leaves a radial part that is rounding noise, and normalising it once pushed
    // the support a full radius off the cap, outside the solid.
    // GJK asks exactly such directions once its search settles onto a flat cap.

    SECTION("hemisphere: a direction against the normal")
    {
        auto const h = tg::hemisphere3d(tg::pos3d(-2.188903399212491, 2.2827678543291974, 0.6006687162097011),
                                        1.9784579831591835,
                                        tg::vec3d(-0.9229696139611775, -0.22734363144288028, 0.3105510665683162));
        auto const p = tg::impl::support_op<tg::hemisphere3d>::apply(
            h, tg::vec3d(1.9344770123648733, 0.47649567470192095, -0.65089239163952883));
        CHECK(tg::dot(p - h.center, h.normal) >= -1e-12);
    }

    SECTION("cylinder: a direction along the axis")
    {
        auto const c
            = tg::cylinder3d(tg::segment3d(tg::pos3d(2.3460813047113582, 1.6326666110792518, 0.8202870125626398),
                                           tg::pos3d(2.387395930482972, 1.3135989079321921, 0.9285868069389394)),
                             1.2687744388304767);
        auto worst = 0.0;
        for (auto k = 1; k <= 64; ++k)
            worst = cc::max(worst, outside_by(c, tg::impl::support_op<tg::cylinder3d>::apply(
                                                     c, (c.axis.pos1 - c.axis.pos0) * (k * 0.37))));
        CHECK(worst <= 1e-12);
    }

    SECTION("cone: a direction along the axis")
    {
        // the axis of the cylinder above
        auto const c = tg::cone3d(tg::pos3d(2.3460813047113582, 1.6326666110792518, 0.8202870125626398),
                                  tg::pos3d(2.387395930482972, 1.3135989079321921, 0.9285868069389394)
                                      - tg::pos3d(2.3460813047113582, 1.6326666110792518, 0.8202870125626398),
                                  1.2687744388304767);
        auto worst = 0.0;
        for (auto k = 1; k <= 64; ++k)
            worst = cc::max(worst, outside_by(c, tg::impl::support_op<tg::cone3d>::apply(c, c.axis * (k * 0.37))));
        CHECK(worst <= 1e-12);
    }
}

TEST("tg gjk - pairs with a capped type meet their certificate")
{
    // The supports above, seen through GJK: each pair once had a closest point outside its solid, or a distance overstated.
    // Each was the first failure of its type pair in the random sweep, with GJK's arguments in this order.

    SECTION("hemisphere and segment: the closest point once lay below the base, by 0.084")
    {
        auto const a = tg::hemisphere3d(tg::pos3d(-2.188903399212491, 2.2827678543291974, 0.6006687162097011),
                                        1.9784579831591835,
                                        tg::vec3d(-0.9229696139611775, -0.22734363144288028, 0.3105510665683162));
        auto const b = tg::segment3d(tg::pos3d(-0.680244266242791, 2.3790869386762687, -2.8732651000376253),
                                     tg::pos3d(-0.07383534340563891, 1.1595917948621959, -0.6845642291070697));
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("hemisphere and aabb: the distance was overstated by 1.29")
    {
        auto const a = tg::hemisphere3d(tg::pos3d(0.17079412733501265, 1.5669173480353606, -1.5812210754985194),
                                        1.6003245008979345,
                                        tg::vec3d(-0.6217871682152392, -0.5484219864050878, -0.559118987578111));
        auto const b = tg::aabb3d(tg::pos3d(0.8192152073216475, 2.318121442058426, 0.08329078286387137),
                                  tg::pos3d(1.9711645466539824, 5.417859216451439, 0.506861495497704));
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("cylinder and triangle: the distance was overstated by 0.29")
    {
        auto const a
            = tg::cylinder3d(tg::segment3d(tg::pos3d(-0.014211257444701975, 0.8577049112087934, 1.6670313424770886),
                                           tg::pos3d(-3.3643230104643833, 1.7760195300045347, 3.003542646240571)),
                             1.2573557763759815);
        auto const b = tg::triangle3d(tg::pos3d(1.1722919427032479, 1.2777596551139951, 0.894079932455969),
                                      tg::pos3d(2.712366873323671, 0.31345490908947404, 1.3017317389138348),
                                      tg::pos3d(1.2302342530958366, -1.7781005224740691, -0.0574830985964061));
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("cylinder and tetrahedron: the distance was overstated by 0.25")
    {
        auto const a
            = tg::cylinder3d(tg::segment3d(tg::pos3d(0.19914055476459422, 2.1757927565355484, -1.3714862439597977),
                                           tg::pos3d(0.2795444099849033, 1.7860294657354654, -2.6736621450422864)),
                             0.8527263275302597);
        auto const b = tg::tetrahedron3d(tg::pos3d(0.10144145018545425, 1.9260942298695025, 0.25117559746780094),
                                         tg::pos3d(0.6056530778774634, 1.5432675787881753, 1.4444943000499904),
                                         tg::pos3d(0.9291659774360645, 1.2542670830254017, 1.5827110370603572),
                                         tg::pos3d(0.29298843500248983, 2.077448628049633, 0.6834151286718941));
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("box and cylinder: the distance was overstated by 0.044")
    {
        auto const a = tg::box3d(
            tg::pos3d(0.10968505583630339, 0.8382729878235775, 0.22663817673088182),
            tg::mat3d::make_from_cols(tg::vec3d(0.12606981311671775, -0.00696254811042561, -0.017518926477819174),
                                      tg::vec3d(-0.05954615360725046, 0.7944032711133967, -0.7442261640745789),
                                      tg::vec3d(0.11908006977236504, 0.5914949214986734, 0.6218469098499794)));
        auto const b
            = tg::cylinder3d(tg::segment3d(tg::pos3d(-3.4292315444080925, -1.0623279647917152, 1.2284515277041217),
                                           tg::pos3d(-0.8613525495881287, -0.43114228161287865, 1.0963904653900018)),
                             1.150868171529278);
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("segment and cone: the closest point once lay outside the cone, by 0.60")
    {
        auto const a = tg::segment3d(tg::pos3d(0.5688737662324281, 0.47627580721257834, -1.6277830438434262),
                                     tg::pos3d(1.824145628714275, 1.2162893234271834, -0.46979005396316964));
        auto const b
            = tg::cone3d(tg::pos3d(-0.4709023861570003, -0.5535504507566054, -1.092104725022323),
                         tg::vec3d(0.27765935930675856, 0.2651217578028849, 0.022170048413677403), 0.9910766419071828);
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("aabb and cone: the distance was overstated by 0.019")
    {
        auto const a = tg::aabb3d(tg::pos3d(1.880628498576185, -2.510871648684855, 1.6212097162579677),
                                  tg::pos3d(3.1457250111295205, -1.445371063312198, 4.225494798143697));
        auto const b
            = tg::cone3d(tg::pos3d(2.366738617597165, 1.473132759505722, 0.8744369097507896),
                         tg::vec3d(0.03384172907222323, -0.2613554537632569, 0.08871077085680275), 1.2687744388304767);
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }

    SECTION("cylinder and sphere: two curved solids, once off by 5e-8")
    {
        auto const a
            = tg::cylinder3d(tg::segment3d(tg::pos3d(2.0838529778530166, 3.2432518809101096, -1.2047711130995262),
                                           tg::pos3d(-0.11203495692136012, 2.702366103326741, -0.4659219355172145)),
                             1.1725734066895406);
        auto const b
            = tg::sphere3d(tg::pos3d(-2.188903399212491, 2.2827678543291974, 0.6006687162097011), 1.9784579831591835);
        CHECK(certificate_residual(a, b) <= certificate_tolerance);
    }
}
