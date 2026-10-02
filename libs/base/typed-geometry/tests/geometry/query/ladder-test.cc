#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The ladder each verb walks, pinned with object types local to this test: a direct kernel beats every derivation,
// a kernel written in the other order still answers, and a pair with nothing at all is refused by the concepts.

namespace
{
/// projects every point to the origin, so the derived distance is |p|²
struct probe_direct
{
};
/// has only a mirrored kernel, (probe_mirror, pos)
struct probe_mirror
{
};
/// has no kernel of any kind
struct probe_none
{
};
} // namespace

template <>
struct tg::impl::project_op<tg::pos2f, probe_direct>
{
    static constexpr tg::pos2f apply(tg::pos2f const&, probe_direct const&) { return tg::pos2f(0, 0); }
};

template <>
struct tg::impl::distance_sqr_op<tg::pos2f, probe_direct>
{
    static constexpr float apply(tg::pos2f const&, probe_direct const&) { return -1.0f; }
};

template <>
struct tg::impl::distance_sqr_op<probe_mirror, tg::pos2f>
{
    static constexpr float apply(probe_mirror const&, tg::pos2f const&) { return -2.0f; }
};

static_assert(tg::has_distance_sqr_to<tg::pos2f, probe_direct>);
static_assert(tg::has_distance_sqr_to<tg::pos2f, probe_mirror>);
static_assert(tg::has_distance_sqr_to<probe_mirror, tg::pos2f>);
static_assert(!tg::has_distance_sqr_to<tg::pos2f, probe_none>);
static_assert(!tg::has_intersects<tg::pos2f, probe_none>);

// the concepts are the probe for real pairs too
static_assert(tg::has_distance_sqr_to<tg::pos3f, tg::triangle3f>);
static_assert(tg::has_distance_sqr_to<tg::triangle3f, tg::pos3f>, "the mirror of a projection is a distance too");
static_assert(tg::has_contains<tg::aabb3f, tg::pos3f>);
static_assert(!tg::has_contains<tg::pos3f, tg::aabb3f>, "contains is not symmetric");
static_assert(tg::has_intersects<tg::sphere3f, tg::pos3f>);
static_assert(!tg::has_project_to<tg::pos3i, tg::segment3i>, "a projection is rational, so an exact scalar has none");
static_assert(tg::has_project_to<tg::pos3i, tg::aabb3i>, "clamping needs only comparisons");

// the probes for the verbs that derive from another: true exactly when the member compiles
static_assert(tg::has_may_intersect<tg::frustum3d, tg::sphere3d>, "the plane-by-plane kernel");
static_assert(tg::has_may_intersect<tg::sphere3d, tg::frustum3d>, "the kernel in the other order");
static_assert(tg::has_may_intersect<tg::sphere3f, tg::pos3f>, "the fallback to intersects");
static_assert(!tg::has_may_intersect<tg::pos2f, probe_none>);
static_assert(tg::has_closest_point_to<tg::aabb3f, tg::pos3f>, "a projection");
static_assert(tg::has_closest_point_to<tg::pos2f, probe_direct>, "closest points derived from a projection");
static_assert(!tg::has_closest_point_to<tg::pos2f, probe_mirror>, "a distance alone gives no point");
static_assert(!tg::has_closest_point_to<tg::pos2f, probe_none>);
static_assert(!tg::has_closest_point_to<tg::pos3i, tg::segment3i>);
static_assert(tg::has_intersects_eps<tg::segment3f, tg::triangle3f>);
static_assert(tg::has_intersects_eps<tg::pos2f, probe_mirror>, "a distance is all it needs");
static_assert(!tg::has_intersects_eps<tg::pos2f, probe_none>);
static_assert(!tg::has_intersects_eps<tg::aabb3i, tg::triangle3i>, "GJK refuses an exact scalar");
static_assert(tg::has_contains_eps<tg::triangle3f, tg::pos3f>);
static_assert(!tg::has_contains_eps<tg::pos3f, tg::triangle3f>, "contains(b, eps) is for a point b");
static_assert(!tg::has_contains_eps<tg::sphere3f, tg::segment3f>, "even where a distance exists");

TEST("tg query - the ladder prefers a direct kernel over every derivation")
{
    auto const p = tg::pos2f(3, 4);

    // the derivation would give |p - origin|² = 25
    CHECK(p.distance_sqr_to(probe_direct()) == -1.0f);
    CHECK(p.project_to(probe_direct()) == tg::pos2f(0, 0));
}

TEST("tg query - a kernel written in the other order answers both")
{
    auto const p = tg::pos2f(3, 4);
    CHECK(p.distance_sqr_to(probe_mirror()) == -2.0f);
}

TEST("tg query - closest points swap back into the caller's order")
{
    auto const p = tg::pos3f(5, 0, 0);
    auto const b = tg::aabb3f(tg::pos3f(-1, -1, -1), tg::pos3f(1, 1, 1));

    auto const [on_p, on_b] = p.closest_points_to(b);
    CHECK(on_p == p);
    CHECK(on_b == tg::pos3f(1, 0, 0));

    auto const [on_b2, on_p2] = b.closest_points_to(p);
    CHECK(on_b2 == tg::pos3f(1, 0, 0));
    CHECK(on_p2 == p);

    CHECK(b.closest_point_to(p) == p.project_to(b));
}
