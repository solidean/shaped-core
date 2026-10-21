#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/scene/quadric_set.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/tetrahedron.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the quadric batch: what `add` accumulates, and what the content hash promises.
//
// The hash is the whole point of the type, so most of this is about it.
// Equal contents must give equal hashes, or a placement uploads bytes the manager already holds; and differing contents must
// give differing hashes, or a placement hands back the WRONG batch — which is the failure that shows up as one drawing wearing
// another's geometry.
// Order is part of "contents" rather than incidental to it: primitive order is what `PrimitiveIndex()` reads, so a per-primitive
// attribute lines up with the order the set was built in.

namespace
{
constexpr float eps = 1e-4f;

bool near(float a, float b, float tol = eps)
{
    return a - b < tol && b - a < tol;
}

/// A set with a few primitives of both kinds, built the way a caller would.
sv::quadric_set structure_of(float radius)
{
    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), radius));
    set.add_sphere(tg::sphere3f(tg::pos3f(1, 0, 0), radius));
    set.add_line(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), radius * 0.5f);
    return set;
}
} // namespace

TEST("sv::quadric_set accumulates primitives and their extent")
{
    auto set = sv::quadric_set();
    CHECK(set.is_empty());
    CHECK(set.primitive_count() == 0);
    CHECK(!set.bounds().has_value()); // nothing to stand in for yet
    CHECK(!set.is_ready());           // nobody has placed it, so nothing has uploaded it

    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
    REQUIRE(set.bounds().has_value());
    CHECK(near(set.bounds().value().min[0], -1.0f));
    CHECK(near(set.bounds().value().max[0], 1.0f));

    // The extent is the union, so a second primitive grows it rather than replacing it.
    set.add_sphere(tg::sphere3f(tg::pos3f(5, 0, 0), 1.0f));
    CHECK(set.primitive_count() == 2);
    CHECK(near(set.bounds().value().min[0], -1.0f));
    CHECK(near(set.bounds().value().max[0], 6.0f));

    // A flat-capped edge is one primitive; the round-capped form is three.
    set.add_line(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 0.1f);
    CHECK(set.primitive_count() == 3);

    set.add_capsule(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 4, 0)), 0.1f);
    CHECK(set.primitive_count() == 6);
}

TEST("sv::quadric_set hashes equal contents equally")
{
    auto const a = structure_of(0.25f);
    auto const b = structure_of(0.25f);

    // Two sets built independently must be one resource, or placing the second uploads what the first already did.
    CHECK(a.hash() == b.hash());
    CHECK(a.primitive_count() == b.primitive_count());
}

TEST("sv::quadric_set hashes differing contents differently")
{
    auto const a = structure_of(0.25f);
    auto const b = structure_of(0.26f);

    // A radius nobody would see the difference of still has to be a different resource.
    CHECK(a.hash() != b.hash());

    // One extra primitive is a different batch too.
    auto c = structure_of(0.25f);
    c.add_sphere(tg::sphere3f(tg::pos3f(9, 9, 9), 0.25f));
    CHECK(c.hash() != a.hash());
}

TEST("sv::quadric_set hashes primitive ORDER")
{
    // Primitive order is what PrimitiveIndex() reads, so a per-primitive attribute lines up with it.
    // Two sets holding the same primitives in a different order therefore shade differently and cannot share a cache entry.
    auto forward = sv::quadric_set();
    forward.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
    forward.add_sphere(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f));

    auto backward = sv::quadric_set();
    backward.add_sphere(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f));
    backward.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));

    CHECK(forward.hash() != backward.hash());

    // The extent is order-independent, which is what makes it a summary rather than part of the identity.
    CHECK(near(forward.bounds().value().min[0], backward.bounds().value().min[0]));
    CHECK(near(forward.bounds().value().max[0], backward.bounds().value().max[0]));
}

TEST("sv::quadric_set hash ignores what is not geometry")
{
    // The hash keys the uploaded PAYLOAD, so two sets differing only in how they are drawn share one.
    // That is what lets a caller recolor a million-primitive batch without re-uploading it.
    auto a = structure_of(0.25f);
    auto b = structure_of(0.25f);

    b.name = "something else";
    b.material = sv::material_id(7);
    b.transform = tg::affine_transform3f(tg::rigid_transform3f::make_translation(tg::vec3f(100, 0, 0)));

    CHECK(a.hash() == b.hash());
}

TEST("sv::quadric_set clears back to empty")
{
    auto set = structure_of(0.25f);
    set.name = "kept";
    set.material = sv::material_id(3);

    auto const empty_hash = sv::quadric_set().hash();
    CHECK(set.hash() != empty_hash);

    set.clear();

    // A per-frame set is refilled rather than rebuilt, so clearing drops the geometry and keeps how it is drawn.
    CHECK(set.is_empty());
    CHECK(!set.bounds().has_value());
    CHECK(set.hash() == empty_hash);
    CHECK(set.name == "kept");
    CHECK(set.material == sv::material_id(3));

    // And refilling reproduces the same key, so a per-frame rebuild of unchanged contents uploads nothing.
    auto refilled = set;
    refilled.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 0.25f));
    refilled.add_sphere(tg::sphere3f(tg::pos3f(1, 0, 0), 0.25f));
    refilled.add_line(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), 0.125f);
    CHECK(refilled.hash() == structure_of(0.25f).hash());
}

TEST("sv::quadric_set carries its primitives in order")
{
    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
    set.add_line(tg::segment3f(tg::pos3f(0, 0, -2), tg::pos3f(0, 0, 2)), 0.5f);

    auto const prims = set.primitives();
    REQUIRE(prims.size() == 2);

    // The sphere is unclipped and the cylinder is not, which is the one structural difference between the two.
    CHECK(prims[0].clip == sv::quadric3::everywhere());
    CHECK(prims[1].clip != sv::quadric3::everywhere());

    // And the cylinder's box is the tight one, so the set's extent is not inflated by a loose primitive.
    CHECK(near(prims[1].bounds.max[2], 2.0f));
}

namespace
{
bool is_sphere(sv::quadric_primitive const& p)
{
    return p.clip == sv::quadric3::everywhere();
}

// A sphere's center and radius, read off its box.
tg::pos3f center_of(sv::quadric_primitive const& p)
{
    return p.bounds.min + (p.bounds.max - p.bounds.min) * 0.5f;
}
} // namespace

static_assert(sv::has_tg_edges<tg::aabb3i> && sv::has_tg_vertices<tg::aabb3i> && sv::has_tg_faces<tg::aabb3i>);
static_assert(sv::has_tg_vertices<tg::segment3f> && !sv::has_tg_edges<tg::segment3f>, "a segment is drawn as a line");
static_assert(sv::has_tg_faces<tg::triangle3d> && sv::has_tg_faces<tg::quad3f>, "a face is its own one face");

TEST("sv::quadric_set draws a tg object's edges, joined once per vertex")
{
    auto const box = tg::aabb3i(tg::pos3i(0, 0, 0), tg::pos3i(2, 2, 2));

    // 12 open tubes, then one sphere per corner: the joints are the box's own 8 vertices, never one per edge end.
    auto set = sv::quadric_set();
    CHECK(set.add_edges(box, {.radius = 0.1f}) == 20);
    auto const prims = set.primitives();
    REQUIRE(prims.size() == 20);
    for (auto i = 0; i < 12; ++i)
    {
        CHECK(!is_sphere(prims[i]));
        CHECK(!prims[i].emits_clip_surface()); // open: the joint sphere is the cap
    }
    auto const corners = box.vertices();
    for (auto i = 0; i < 8; ++i)
    {
        REQUIRE(is_sphere(prims[12 + i]));
        CHECK(center_of(prims[12 + i]) == sv::impl::to_pos3f(corners[i]));
        CHECK(near(prims[12 + i].bounds.max[0] - center_of(prims[12 + i])[0], 0.1f)); // the tube's radius: exactly round
    }

    // A larger vertex radius stands the corners out; without vertices, each tube is closed on its own.
    auto big = sv::quadric_set();
    (void)big.add_edges(box, {.radius = 0.1f, .vertex_radius = 0.3f});
    CHECK(near(big.primitives()[12].bounds.max[0] - center_of(big.primitives()[12])[0], 0.3f));

    auto bare = sv::quadric_set();
    CHECK(bare.add_edges(box, {.vertices = false}) == 12);
    CHECK(bare.primitives()[0].emits_clip_surface()); // flat ends by default
}

TEST("sv::quadric_set draws a tg object's vertices")
{
    auto const tet = tg::tetrahedron3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0), tg::pos3f(0, 0, 1));
    auto set = sv::quadric_set();
    CHECK(set.add_vertices(tet, {.radius = 0.05f}) == 4);
    CHECK(center_of(set.primitives()[3]) == tet.pos3);

    // a triangle's edges and corners: three of each
    CHECK(set.add_edges(tg::triangle3f(tet.pos0, tet.pos1, tet.pos2)) == 6);
}

TEST("sv::triangle_geometry draws a tg object's faces")
{
    // an integer box: 6 quads, 2 triangles each, every one facing away from the center
    auto const g = sv::triangle_geometry::create_from_faces(tg::aabb3i(tg::pos3i(0, 0, 0), tg::pos3i(1, 1, 1)));
    REQUIRE(g.triangle_count() == 12);
    auto const p = g.positions.span();
    for (auto t = 0; t < 12; ++t)
    {
        auto const n = tg::dual(tg::cross(p[3 * t + 1] - p[3 * t], p[3 * t + 2] - p[3 * t]));
        CHECK(tg::dot(n, p[3 * t] - tg::pos3f(0.5f, 0.5f, 0.5f)) > 0.0f);
    }

    // a range of objects, and faces that are objects themselves
    auto tets = cc::vector<tg::tetrahedron3d>();
    tets.push_back(tg::tetrahedron3d(tg::pos3d(0, 0, 0), tg::pos3d(1, 0, 0), tg::pos3d(0, 1, 0), tg::pos3d(0, 0, 1)));
    tets.push_back(tets[0]);
    CHECK(sv::triangle_geometry::create_from_faces(tets).triangle_count() == 8);
    CHECK(sv::triangle_geometry::create_from_faces(tg::triangle3f()).triangle_count() == 1);
    CHECK(sv::triangle_geometry::create_from_faces(tg::quad3f()).triangle_count() == 2);
}
