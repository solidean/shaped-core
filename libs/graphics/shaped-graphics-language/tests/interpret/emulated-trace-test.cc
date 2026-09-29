#include "driver-test-support.hh"

using namespace sgl_test;

// The emulated trace run on the interpreter, against pools written by hand as the internal doc raytracing-polyfill.md lays them out.
// Every expectation is worked out from the geometry by hand.
// No ray passes through an edge or a corner, since the triangle test is not watertight yet.

namespace
{
struct float3
{
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
};

struct triangle
{
    float3 v0;
    float3 v1;
    float3 v2;
};

/// A box as the pool stores one.
struct box
{
    float3 lo;
    float3 hi;
};

struct instance
{
    u32 blas = 0;
    /// Object to world is `p * scale + translation`.
    f32 scale = 1;
    float3 translation;
    u32 id = 0;
    u32 mask = 0xFF;
    /// Bits 24 to 31 of unit 6's z: bit 0 forced opaque, bit 1 forced non-opaque, bits 2 and 3 the cull mode.
    u32 flags = 0;
};

/// A pool as sg's webgpu backend writes it: units of four words, unit 0 the empty TLAS.
struct pool_writer
{
    cc::vector<u32> words = {0, 0, 0, 0};

    [[nodiscard]] u32 size() const { return u32(words.size() / 4); }

    u32 unit(u32 x, u32 y, u32 z, u32 w)
    {
        auto const at = size();
        words.push_back(x);
        words.push_back(y);
        words.push_back(z);
        words.push_back(w);
        return at;
    }

    u32 point(float3 p, u32 w) { return unit(bits_of(p.x), bits_of(p.y), bits_of(p.z), w); }

    void row(f32 a, f32 b, f32 c, f32 d) { unit(bits_of(a), bits_of(b), bits_of(c), bits_of(d)); }

    /// One leaf of at most 4 primitives: the root, which a region's first node always is.
    void leaf(box b, u32 count)
    {
        point(b.lo, 0);
        point(b.hi, count | 0x80000000u);
    }

    /// A BLAS of one geometry, index `geometry`, whose root leaf has the box `root`.
    u32 blas(cc::span<triangle const> triangles, bool is_opaque, box root, u32 geometry = 0)
    {
        auto const at = unit(1, size() + 3, u32(triangles.size()), 0);
        leaf(root, u32(triangles.size()));
        for (auto i = isize(0); i < triangles.size(); ++i)
        {
            point(triangles[i].v0, u32(i));
            point(triangles[i].v1, geometry);
            point(triangles[i].v2, is_opaque ? 1 : 0);
        }
        return at;
    }

    /// A BLAS of procedural boxes, one geometry, index `geometry`, whose root leaf has the box `root`.
    u32 blas_of_boxes(cc::span<box const> boxes, bool is_opaque, box root, u32 geometry = 0)
    {
        auto const at = unit(1, size() + 3, u32(boxes.size()), 1);
        leaf(root, u32(boxes.size()));
        for (auto i = isize(0); i < boxes.size(); ++i)
        {
            point(boxes[i].lo, u32(i));
            point(boxes[i].hi, geometry | (is_opaque ? 0x80000000u : 0u));
        }
        return at;
    }

    /// A TLAS whose root leaf has the box `root`; `kinds` is bit 0 for triangles and bit 1 for boxes.
    u32 tlas(cc::span<instance const> instances, box root, u32 kinds = 1)
    {
        auto const at = unit(1, size() + 3, u32(instances.size()), kinds);
        leaf(root, u32(instances.size()));
        for (auto i = isize(0); i < instances.size(); ++i)
        {
            auto const& n = instances[i];
            auto const s = n.scale;
            auto const t = n.translation;
            row(1 / s, 0, 0, -t.x / s);
            row(0, 1 / s, 0, -t.y / s);
            row(0, 0, 1 / s, -t.z / s);
            row(s, 0, 0, t.x);
            row(0, s, 0, t.y);
            row(0, 0, s, t.z);
            unit(n.blas, n.id | n.mask << 24, n.flags << 24, u32(i));
        }
        return at;
    }
};

// Two triangles over the unit square's lower left, primitive 0 at z = 5 and primitive 1 nearer at z = 2.
// Along +z each is front facing: DXR's front is clockwise as the ray sees it.
constexpr triangle k_pair[] = {
    {{0, 0, 5}, {1, 0, 5}, {0, 1, 5}},
    {{0, 0, 2}, {1, 0, 2}, {0, 1, 2}},
};
constexpr box k_pair_box = {{0, 0, 2}, {1, 1, 5}};

struct scenes
{
    pool_writer pool;
    u32 pair = 0;
    u32 instanced = 0;
    u32 cutout = 0;
    u32 boxed = 0;
    u32 mixed = 0;
    u32 ball = 0;
};

// One box around the unit sphere at (3, 1, 1), which the tests' intersection reports.
constexpr box k_ball_box = {{2, 0, 0}, {4, 2, 2}};

scenes build_scenes()
{
    auto s = scenes();
    auto const opaque = s.pool.blas(k_pair, true, k_pair_box);

    // the pair where it stands, as instance 0 with id 7
    instance const pair[] = {{.blas = opaque, .id = 7}};
    s.pair = s.pool.tlas(pair, k_pair_box);

    // instance 1 of two, id 42: the pair scaled by 2 and moved to x = 10, so its box is (10, 0, 4) to (12, 2, 10)
    instance const instanced[] = {
        {.blas = opaque, .translation = {100, 100, 100}, .id = 5},
        {.blas = opaque, .scale = 2, .translation = {10, 0, 0}, .id = 42},
    };
    s.instanced = s.pool.tlas(instanced, {{10, 0, 4}, {101, 101, 105}});

    // the pair forced non-opaque by its instance, so the any-hit decision sees every triangle
    instance const cutout[] = {{.blas = opaque, .id = 9, .flags = 2}};
    s.cutout = s.pool.tlas(cutout, k_pair_box);

    // a root box far from the pair: a ray that misses it tests no triangle, whatever lies on its way
    instance const boxed[] = {{.blas = opaque, .id = 3}};
    s.boxed = s.pool.tlas(boxed, {{50, 50, 50}, {51, 51, 51}});

    // both kinds: the pair where it stands (id 7), the ball's box, non-opaque (id 11), and the pair again, forced
    // non-opaque and moved behind the ball to x = 2.5, y = 0.5 (id 13)
    box const balls[] = {k_ball_box};
    auto const ball = s.pool.blas_of_boxes(balls, false, k_ball_box);
    instance const mixed[] = {
        {.blas = opaque, .id = 7},
        {.blas = ball, .id = 11},
        {.blas = opaque, .translation = {2.5f, 0.5f, 0}, .id = 13, .flags = 2},
    };
    s.mixed = s.pool.tlas(mixed, {{0, 0, 0}, {4, 2, 5}}, 3);

    // the ball alone
    instance const ball_only[] = {{.blas = ball, .id = 11}};
    s.ball = s.pool.tlas(ball_only, k_ball_box, 2);
    return s;
}

sgl::check::driver_bindings bindings_of(scenes const& s)
{
    return {
        .groups = {{
            .name = "scenes",
            .members = {
                {.name = "empty", .acceleration_root = 0},
                {.name = "pair", .acceleration_root = s.pair},
                {.name = "instanced", .acceleration_root = s.instanced},
                {.name = "cutout", .acceleration_root = s.cutout},
                {.name = "boxed", .acceleration_root = s.boxed},
                {.name = "broken", .acceleration_root = 100000},
                {.name = "mixed", .acceleration_root = s.mixed},
                {.name = "ball", .acceleration_root = s.ball},
            },
        }},
        .acceleration_pool = bytes_of(s.pool.words),
    };
}

constexpr cc::string_view k_declarations = "require ray_query\n"
                                           "binding scenes:\n"
                                           "    empty: acceleration_structure[.triangles]\n"
                                           "    pair: acceleration_structure[.triangles]\n"
                                           "    instanced: acceleration_structure[.triangles]\n"
                                           "    cutout: acceleration_structure[.triangles]\n"
                                           "    boxed: acceleration_structure[.triangles]\n"
                                           "    broken: acceleration_structure[.triangles]\n"
                                           "    mixed: acceleration_structure[.mixed]\n"
                                           "    ball: acceleration_structure[.procedural]\n"
                                           "fun is_near(a: float, b: float) -> bool => abs(a - b) < 1.0e-5\n"
                                           "fun along_z(x: float, y: float) -> ray => ray(origin = pos3(x, y, -1.0), "
                                           "direction = vec3(0.0, 0.0, 1.0))\n";

/// Every failing test of `tests`, run behind the declarations against the scenes.
cc::string failures_of(cc::string_view tests)
{
    auto source = cc::string(k_declarations);
    source += tests;
    return driven_failures_of(source, bindings_of(build_scenes()));
}
} // namespace

TEST("sgl emulated trace - a trace of the empty TLAS misses, and its t is the ray's end")
{
    CHECK(failures_of("test {scenes}:\n"
                      "    let h = scenes.empty.trace(along_z(0.25, 0.25))\n"
                      "    not h.is_hit\n"
                      "    h.t == 3.0e38\n")
          == "");
}

TEST("sgl emulated trace - the nearest of two triangles is the hit, with its t, ids and barycentrics")
{
    // (0.25, 0.25) weighs vertex 1 by its x and vertex 2 by its y, and the rest is vertex 0's
    CHECK(failures_of("test {scenes}:\n"
                      "    let h = scenes.pair.trace(along_z(0.25, 0.25))\n"
                      "    h.is_hit\n"
                      "    is_near(h.t, 3.0)\n"
                      "    h.primitive_index == 1 and h.geometry_index == 0\n"
                      "    h.instance_id == 7 and h.instance_index == 0\n"
                      "    is_near(h.barycentrics.x, 0.5) and is_near(h.barycentrics.y, 0.25) and "
                      "is_near(h.barycentrics.z, 0.25)\n"
                      "    h.is_front_face\n"
                      // a ray that ends before the nearer triangle reaches only the farther one, and one past both none
                      "test {scenes}:\n"
                      "    let r = along_z(0.25, 0.25)\n"
                      "    let h = scenes.pair.trace(ray(origin = r.origin, direction = r.direction, t_min = 4.0))\n"
                      "    h.is_hit and h.primitive_index == 0 and is_near(h.t, 6.0)\n"
                      "    not scenes.pair.trace(along_z(0.75, 0.75)).is_hit\n"
                      "    not scenes.pair.trace(along_z(0.25, 0.25), mask = 0).is_hit\n")
          == "");
}

TEST("sgl emulated trace - an instance moves the ray into object space, and the hit back out")
{
    // x = 10.5 is 0.25 in the object, and z = 2 there is 4 in the world: t = 5 from z = -1
    CHECK(failures_of("test {scenes}:\n"
                      "    let h = scenes.instanced.trace(along_z(10.5, 0.5))\n"
                      "    h.is_hit\n"
                      "    is_near(h.t, 5.0)\n"
                      "    h.instance_id == 42 and h.instance_index == 1 and h.primitive_index == 1\n"
                      "    is_near(h.barycentrics.y, 0.25) and is_near(h.barycentrics.z, 0.25)\n"
                      "    let p = h.to_world(pos3(0.0, 0.0, 2.0))\n"
                      "    is_near(p.x, 10.0) and is_near(p.y, 0.0) and is_near(p.z, 4.0)\n"
                      "    let q = h.to_object(pos3(10.5, 0.5, 4.0))\n"
                      "    is_near(q.x, 0.25) and is_near(q.y, 0.25) and is_near(q.z, 2.0)\n"
                      // the other instance is where its translation put it, and the ray at the old place hits nothing
                      "test {scenes}:\n"
                      "    scenes.instanced.trace(ray(origin = pos3(100.25, 100.25, 0.0), direction = vec3(0.0, 0.0, "
                      "1.0))).instance_id == 5\n"
                      "    not scenes.instanced.trace(along_z(0.25, 0.25)).is_hit\n")
          == "");
}

TEST("sgl emulated trace - the any-hit decision cuts a non-opaque triangle out, and an opaque one never asks")
{
    CHECK(failures_of("fun cut_near(c: triangle_candidate) -> hit_decision:\n"
                      "    if c.primitive_index == 1 => return hit_decision.ignore\n"
                      "    return hit_decision.accept\n"
                      "test {scenes}:\n"
                      "    let h = scenes.cutout.trace(along_z(0.25, 0.25), cut_near)\n"
                      "    h.is_hit and h.primitive_index == 0 and is_near(h.t, 6.0)\n"
                      // opaque geometry is accepted without a decision, unless the ray forces it non-opaque
                      "    scenes.pair.trace(along_z(0.25, 0.25), cut_near).primitive_index == 1\n"
                      "    scenes.pair.trace(along_z(0.25, 0.25), cut_near, flags = "
                      "ray_flags.force_non_opaque).primitive_index == 0\n"
                      // and a ray forced opaque decides nothing, even through the non-opaque instance
                      "    scenes.cutout.trace(along_z(0.25, 0.25), cut_near, flags = "
                      "ray_flags.force_opaque).primitive_index == 1\n")
          == "");
}

TEST("sgl emulated trace - a shadow ray is occluded by anything before its end")
{
    CHECK(failures_of("test {scenes}:\n"
                      "    is_occluded(scenes.pair, along_z(0.25, 0.25))\n"
                      "    let r = along_z(0.25, 0.25)\n"
                      "    not is_occluded(scenes.pair, ray(origin = r.origin, direction = r.direction, t_max = 2.5))\n"
                      "    not is_occluded(scenes.pair, along_z(0.75, 0.75))\n"
                      "    not is_occluded(scenes.empty, r)\n")
          == "");
}

TEST("sgl emulated trace - a ray that misses the root box meets nothing inside it")
{
    // the pair lies on the ray, and only the root box says it is elsewhere: traversal trusts the box
    CHECK(failures_of("test {scenes}:\n"
                      "    not scenes.boxed.trace(along_z(0.25, 0.25)).is_hit\n"
                      "    scenes.boxed.trace(ray(origin = pos3(50.5, 50.5, 0.0), direction = vec3(0.0, 0.0, 1.0))).t "
                      "== 3.0e38\n")
          == "");
}

TEST("sgl emulated trace - a root past the end of the pool is a program error")
{
    CHECK(failures_of("test {scenes}:\n"
                      "    not scenes.broken.trace(along_z(0.25, 0.25)).is_hit\n")
              .contains("the unit 100000 is past the end of an acceleration pool of"));
}

namespace
{
// The mixed scene's ball: the unit sphere at (3, 1, 1), in a box its instance leaves where it was built.
constexpr cc::string_view k_ball
    = "struct ball:\n"
      "    normal: vec3\n"
      "fun ball_report(b: procedural_box) -> report[ball]:\n"
      "    let o = float3(b.object_ray.origin.x, b.object_ray.origin.y, b.object_ray.origin.z)\n"
      "    let d = float3(b.object_ray.direction.x, b.object_ray.direction.y, "
      "b.object_ray.direction.z)\n"
      "    let oc = o - float3(3.0, 1.0, 1.0)\n"
      "    let half_b = dot(oc, d)\n"
      "    let disc = half_b * half_b - dot(d, d) * (dot(oc, oc) - 1.0)\n"
      "    if disc < 0.0 => return report.none()\n"
      "    let t = (-half_b - sqrt(disc)) / dot(d, d)\n"
      "    let p = oc + d * t\n"
      "    return report(t = t, attributes = ball(vec3(p.x, p.y, p.z)))\n"
      "fun no_ball(c: procedural_candidate[ball]) -> hit_decision => hit_decision.ignore\n"
      "fun accept_all(c: triangle_candidate) -> hit_decision => hit_decision.accept\n"
      "fun cut_near(c: triangle_candidate) -> hit_decision:\n"
      "    if c.primitive_index == 1 => return hit_decision.ignore\n"
      "    return hit_decision.accept\n";

/// Every failing test of `tests`, run behind the declarations and the ball against the scenes.
cc::string mixed_failures_of(cc::string_view tests)
{
    auto source = cc::string(k_ball);
    source += tests;
    return failures_of(source);
}
} // namespace

TEST("sgl emulated trace - a procedural trace reports the ball, and its miss carries undefined attributes")
{
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let h = scenes.ball.trace(along_z(2.75, 0.75), ball_report)\n"
                            "    h.is_hit and is_near(h.t, 2.0 - sqrt(0.875)) and h.instance_id == 11\n"
                            "    is_near(h.attributes.normal.z, -sqrt(0.875))\n"
                            "    not scenes.ball.trace(along_z(2.75, 0.75), ball_report, no_ball).is_hit\n"
                            "    scenes.ball.trace(along_z(0.25, 0.25), ball_report).t == 3.0e38\n")
          == "");
}

TEST("sgl emulated trace - a mixed trace hits a triangle, and gives it as a triangle's hit")
{
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let h = scenes.mixed.trace(along_z(0.25, 0.25), ball_report)\n"
                            "    h.is_hit and h.kind == hit_kind.triangle\n"
                            "    is_near(h.t, 3.0)\n"
                            "    h.instance_id == 7 and h.instance_index == 0 and h.primitive_index == 1\n"
                            "    is_near(h.barycentrics.y, 0.25) and is_near(h.barycentrics.z, 0.25)\n"
                            "    h.is_front_face\n"
                            "    let t = h.triangle()\n"
                            "    t.is_hit and is_near(t.t, 3.0) and t.primitive_index == 1\n"
                            "    not h.procedural().is_hit\n")
          == "");
}

TEST("sgl emulated trace - a mixed trace takes the nearer of a box's report and a triangle behind it")
{
    // the ball stands before the moved pair, whose nearer triangle is at z = 2
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let h = scenes.mixed.trace(along_z(2.75, 0.75), ball_report)\n"
                            "    h.is_hit and h.kind == hit_kind.procedural\n"
                            "    is_near(h.t, 2.0 - sqrt(0.875))\n"
                            "    h.instance_id == 11 and h.instance_index == 1\n"
                            "    h.primitive_index == 0 and h.geometry_index == 0\n"
                            "    is_near(h.attributes.normal.x, -0.25) and is_near(h.attributes.normal.y, -0.25)\n"
                            "    is_near(h.attributes.normal.z, -sqrt(0.875))\n"
                            "    let p = h.procedural()\n"
                            "    p.is_hit and p.instance_id == 11 and is_near(p.attributes.normal.x, -0.25)\n"
                            "    not h.triangle().is_hit\n"
                            "    let q = h.to_world(pos3(3.0, 1.0, 1.0))\n"
                            "    is_near(q.x, 3.0) and is_near(q.y, 1.0) and is_near(q.z, 1.0)\n")
          == "");
}

TEST("sgl emulated trace - a mixed trace asks each kind's decision of its own candidates")
{
    // the ball cut out, the moved pair behind it is the hit, and its non-opaque triangles are decided
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let r = along_z(2.75, 0.75)\n"
                            "    let h = scenes.mixed.trace(r, any_hit = accept_all, intersection = ball_report, "
                            "procedural_any_hit = no_ball)\n"
                            "    h.kind == hit_kind.triangle and h.instance_id == 13 and h.primitive_index == 1\n"
                            "    is_near(h.t, 3.0)\n"
                            "    let cut = scenes.mixed.trace(r, any_hit = cut_near, intersection = ball_report, "
                            "procedural_any_hit = no_ball)\n"
                            "    cut.kind == hit_kind.triangle and cut.primitive_index == 0 and is_near(cut.t, 6.0)\n"
                            // the pair where it stands is opaque, and no decision is asked of it
                            "    scenes.mixed.trace(along_z(0.25, 0.25), any_hit = cut_near, intersection = "
                            "ball_report, "
                            "procedural_any_hit = no_ball).primitive_index == 1\n")
          == "");
}

TEST("sgl emulated trace - a mixed trace misses where nothing is, and skips the kind its flags skip")
{
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let h = scenes.mixed.trace(along_z(1.5, 1.5), ball_report)\n"
                            "    h.kind == hit_kind.none and h.t == 3.0e38 and not h.is_hit\n"
                            "    let r = along_z(2.75, 0.75)\n"
                            "    let s = scenes.mixed.trace(r, ball_report, flags = ray_flags.skip_procedural)\n"
                            "    s.kind == hit_kind.triangle and s.instance_id == 13\n"
                            "    scenes.mixed.trace(r, ball_report, flags = ray_flags.skip_triangles).kind == "
                            "hit_kind.procedural\n"
                            "    not scenes.mixed.trace(along_z(0.25, 0.25), ball_report, flags = "
                            "ray_flags.skip_triangles).is_hit\n"
                            "    not scenes.mixed.trace(r, ball_report, mask = 0).is_hit\n")
          == "");
}
