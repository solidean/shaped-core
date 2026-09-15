#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/scene/quadric_set.hh>
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
    set.add(tg::sphere3f(tg::pos3f(0, 0, 0), radius));
    set.add(tg::sphere3f(tg::pos3f(1, 0, 0), radius));
    set.add(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), radius * 0.5f);
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

    set.add(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
    REQUIRE(set.bounds().has_value());
    CHECK(near(set.bounds().value().min[0], -1.0f));
    CHECK(near(set.bounds().value().max[0], 1.0f));

    // The extent is the union, so a second primitive grows it rather than replacing it.
    set.add(tg::sphere3f(tg::pos3f(5, 0, 0), 1.0f));
    CHECK(set.primitive_count() == 2);
    CHECK(near(set.bounds().value().min[0], -1.0f));
    CHECK(near(set.bounds().value().max[0], 6.0f));

    // A flat-capped edge is one primitive; the round-capped form is three.
    set.add(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 0.1f);
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
    c.add(tg::sphere3f(tg::pos3f(9, 9, 9), 0.25f));
    CHECK(c.hash() != a.hash());
}

TEST("sv::quadric_set hashes primitive ORDER")
{
    // Primitive order is what PrimitiveIndex() reads, so a per-primitive attribute lines up with it.
    // Two sets holding the same primitives in a different order therefore shade differently and cannot share a cache entry.
    auto forward = sv::quadric_set();
    forward.add(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
    forward.add(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f));

    auto backward = sv::quadric_set();
    backward.add(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f));
    backward.add(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));

    CHECK(forward.hash() != backward.hash());

    // The extent is order-independent, which is what makes it a summary rather than part of the identity.
    CHECK(near(forward.bounds().value().min[0], backward.bounds().value().min[0]));
    CHECK(near(forward.bounds().value().max[0], backward.bounds().value().max[0]));
}

TEST("sv::quadric_set hash ignores what is not geometry")
{
    // The hash keys the uploaded PAYLOAD, so two sets differing only in how they are drawn share one.
    // That is what lets a caller recolour a million-primitive batch without re-uploading it.
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
    refilled.add(tg::sphere3f(tg::pos3f(0, 0, 0), 0.25f));
    refilled.add(tg::sphere3f(tg::pos3f(1, 0, 0), 0.25f));
    refilled.add(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), 0.125f);
    CHECK(refilled.hash() == structure_of(0.25f).hash());
}

TEST("sv::quadric_set carries its primitives in order")
{
    auto set = sv::quadric_set();
    set.add(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
    set.add(tg::segment3f(tg::pos3f(0, 0, -2), tg::pos3f(0, 0, 2)), 0.5f);

    auto const prims = set.primitives();
    REQUIRE(prims.size() == 2);

    // The sphere is unclipped and the cylinder is not, which is the one structural difference between the two.
    CHECK(prims[0].clip == sv::quadric3::everywhere());
    CHECK(prims[1].clip != sv::quadric3::everywhere());

    // And the cylinder's box is the tight one, so the set's extent is not inflated by a loose primitive.
    CHECK(near(prims[1].bounds.max[2], 2.0f));
}
