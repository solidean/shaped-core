#pragma once

#include "driver-test-support.hh"

// The scenes the emulated walk runs against (EVAL-95), as pools written by hand the way the internal doc
// libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md lays them out.
// A trace's tests and a query's share them, so a query's answer is held to the trace's on the same geometry.

namespace sgl_test::emulated_scenes
{
struct float3;
struct triangle;
struct box;
struct instance;
struct pool_writer;
struct scenes;
} // namespace sgl_test::emulated_scenes

struct sgl_test::emulated_scenes::float3
{
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
};

struct sgl_test::emulated_scenes::triangle
{
    float3 v0;
    float3 v1;
    float3 v2;
};

/// A box as the pool stores one.
struct sgl_test::emulated_scenes::box
{
    float3 lo;
    float3 hi;
};

struct sgl_test::emulated_scenes::instance
{
    u32 blas = 0;
    /// Object to world is `p * scale + translation`.
    f32 scale = 1;
    float3 translation;
    u32 id = 0;
    u32 mask = 0xFF;
    /// Bits 24 to 31 of unit 6's z: bit 0 forced opaque, bit 1 forced non-opaque, bits 2 and 3 the cull mode.
    u32 flags = 0;
    /// Where the translation has moved to at time 1, which makes the instance a moving one (EVAL-130).
    cc::optional<float3> end_translation = {};
};

/// A pool as sg's webgpu backend writes it: units of four words, unit 0 the empty TLAS.
struct sgl_test::emulated_scenes::pool_writer
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

    /// A BLAS whose triangles move from `start` to `end`: its header's w sets bit 2, and each triangle takes 6 units.
    /// `root` holds both poses.
    u32 moving_blas(cc::span<triangle const> start, cc::span<triangle const> end, bool is_opaque, box root)
    {
        auto const at = unit(1, size() + 3, u32(start.size()), 4);
        leaf(root, u32(start.size()));
        for (auto i = isize(0); i < start.size(); ++i)
        {
            point(start[i].v0, u32(i));
            point(start[i].v1, 0);
            point(start[i].v2, is_opaque ? 1 : 0);
            point(end[i].v0, 0);
            point(end[i].v1, 0);
            point(end[i].v2, 0);
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
    /// A TLAS with a moving instance sets its header's kinds bit 2, and every instance of it takes 10 units.
    u32 tlas(cc::span<instance const> instances, box root, u32 kinds = 1)
    {
        auto is_moving = false;
        for (auto const& n : instances)
            is_moving = is_moving || n.end_translation.has_value();
        auto const at = unit(1, size() + 3, u32(instances.size()), kinds | (is_moving ? 4u : 0u));
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
            auto const moving_flag = n.end_translation.has_value() ? 1u << 4 : 0u;
            unit(n.blas, n.id | n.mask << 24, (n.flags | moving_flag) << 24, u32(i));
            if (is_moving)
            {
                auto const e = n.end_translation.has_value() ? n.end_translation.value() : t;
                row(s, 0, 0, e.x);
                row(0, s, 0, e.y);
                row(0, 0, s, e.z);
            }
        }
        return at;
    }
};

namespace sgl_test::emulated_scenes
{
// Two triangles over the unit square's lower left, primitive 0 at z = 5 and primitive 1 nearer at z = 2.
// Along +z each is a back face: its counterclockwise side faces -z, away from the ray, and a front face turns that side to it.
// That is how dx12 and vulkan hardware report it: DXR's "clockwise from the ray's origin" is stated in a left-handed space.
constexpr triangle k_pair[] = {
    {{0, 0, 5}, {1, 0, 5}, {0, 1, 5}},
    {{0, 0, 2}, {1, 0, 2}, {0, 1, 2}},
};
constexpr box k_pair_box = {{0, 0, 2}, {1, 1, 5}};
} // namespace sgl_test::emulated_scenes

struct sgl_test::emulated_scenes::scenes
{
    pool_writer pool;
    u32 pair = 0;
    u32 instanced = 0;
    u32 cutout = 0;
    u32 boxed = 0;
    u32 mixed = 0;
    u32 ball = 0;
    u32 flipped = 0;
    u32 flipped_mixed = 0;
    u32 moving = 0;
    u32 morphing = 0;
    u32 morphing_moved = 0;
    u32 morphing_mixed = 0;
    u32 sheet = 0;
    u32 sheet_flipped = 0;
    u32 abutting = 0;
    u32 tiny = 0;
    u32 decals = 0;
};

namespace sgl_test::emulated_scenes
{
/// A bumpy sheet over the 4 by 4 cells from (0, 0): a height on each corner, so no two neighbours share a plane.
/// Its diagonals alternate, and with `is_flipped` every other triangle winds the other way.
/// So a corner of it is met by four triangles or by eight, and an edge by two, of either winding.
inline cc::vector<triangle> sheet_triangles(bool is_flipped)
{
    auto const corner = [](int x, int y) { return float3{f32(x), f32(y), 2 + f32((x * 7 + y * 3) % 5) * 0.25f}; };
    auto out = cc::vector<triangle>();
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            auto const a = corner(x, y);
            auto const b = corner(x + 1, y);
            auto const c = corner(x, y + 1);
            auto const d = corner(x + 1, y + 1);
            auto const first = (x + y) % 2 == 0 ? triangle{a, b, d} : triangle{a, b, c};
            auto const second = (x + y) % 2 == 0 ? triangle{a, d, c} : triangle{b, d, c};
            out.push_back(first);
            out.push_back(is_flipped ? triangle{second.v0, second.v2, second.v1} : second);
        }
    return out;
}

// One box around the unit sphere at (3, 1, 1), which the tests' intersection reports.
constexpr box k_ball_box = {{2, 0, 0}, {4, 2, 2}};

inline scenes build_scenes()
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

    // the pair with cull mode 1, front, which flips its winding: each native backend sets its front-counterclockwise flag
    instance const flipped[] = {{.blas = opaque, .id = 17, .flags = 1u << 2}};
    s.flipped = s.pool.tlas(flipped, k_pair_box);
    s.flipped_mixed = s.pool.tlas(flipped, k_pair_box, 3);

    // the pair moving from where it stands at time 0 to x = 10 at time 1 (id 21), beside a static copy at y = 50
    // (id 22), which the moving TLAS strides over by 10 units too; the root box holds the whole sweep
    instance const moving[] = {
        {.blas = opaque, .id = 21, .end_translation = float3{10, 0, 0}},
        {.blas = opaque, .translation = {0, 50, 0}, .id = 22},
    };
    s.moving = s.pool.tlas(moving, {{0, 0, 2}, {11, 51, 5}});

    // Primitive 0 falls from z = 5 to z = 1 and leans: its second vertex goes from x = 1 to x = 3.
    // Primitive 1 stands at x = 20 in both poses, so one structure holds a moving triangle and a still one.
    constexpr triangle morph_start[] = {
        {{0, 0, 5}, {1, 0, 5}, {0, 1, 5}},
        {{20, 0, 5}, {21, 0, 5}, {20, 1, 5}},
    };
    constexpr triangle morph_end[] = {
        {{0, 0, 1}, {3, 0, 1}, {0, 1, 1}},
        {{20, 0, 5}, {21, 0, 5}, {20, 1, 5}},
    };
    auto const morphing = s.pool.moving_blas(morph_start, morph_end, true, {{0, 0, 1}, {21, 1, 5}});
    instance const morphs[] = {{.blas = morphing, .id = 31}};
    s.morphing = s.pool.tlas(morphs, {{0, 0, 1}, {21, 1, 5}});

    // the same vertices in an instance that moves too, from where it stands to x = 10
    instance const morphs_moved[] = {{.blas = morphing, .id = 32, .end_translation = float3{10, 0, 0}}};
    s.morphing_moved = s.pool.tlas(morphs_moved, {{0, 0, 1}, {31, 1, 5}});

    // beside the ball's box, for the mixed walk
    instance const morphs_mixed[] = {{.blas = morphing, .id = 33}, {.blas = ball, .id = 11}};
    s.morphing_mixed = s.pool.tlas(morphs_mixed, {{0, 0, 0}, {21, 2, 5}}, 3);

    constexpr box sheet_box = {{0, 0, 2}, {4, 4, 3}};
    instance const sheet[] = {{.blas = s.pool.blas(sheet_triangles(false), false, sheet_box), .id = 41}};
    s.sheet = s.pool.tlas(sheet, sheet_box);
    instance const sheet_flipped[] = {{.blas = s.pool.blas(sheet_triangles(true), false, sheet_box), .id = 42}};
    s.sheet_flipped = s.pool.tlas(sheet_flipped, sheet_box);

    // Two triangles meeting along x = 1 with a structure each, whose boxes' sides a ray along that edge runs along.
    constexpr triangle left[] = {{{0, 0, 2}, {1, 0, 2}, {1, 1, 2}}};
    constexpr box left_box = {{0, 0, 2}, {1, 1, 2}};
    constexpr triangle right[] = {{{0, 0, 2}, {1, 1, 2}, {0, 1, 2}}};
    constexpr box right_box = {{0, 0, 2}, {1, 1, 2}};
    instance const abutting[] = {
        {.blas = s.pool.blas(left, true, left_box), .id = 51},
        {.blas = s.pool.blas(right, true, right_box), .translation = {1, 0, 0}, .id = 52},
    };
    s.abutting = s.pool.tlas(abutting, {{0, 0, 2}, {2, 1, 2}});

    constexpr triangle tiny[] = {{{0, 0, 2}, {1e-7f, 0, 2}, {0, 1e-7f, 2}}};
    constexpr box tiny_box = {{0, 0, 2}, {1e-7f, 1e-7f, 2}};
    instance const tiny_instance[] = {{.blas = s.pool.blas(tiny, true, tiny_box), .id = 61}};
    s.tiny = s.pool.tlas(tiny_instance, tiny_box);

    // Decals: the unit box around the origin, opaque, placed as four instances of mask 2 and one of mask 1.
    // Ids 100 and 101 overlap around (10.1, 10, 10), 102 stands alone, and 103 of mask 1 covers 100 again.
    constexpr box unit_box = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    box const decal_boxes[] = {unit_box};
    auto const decal = s.pool.blas_of_boxes(decal_boxes, true, unit_box);
    instance const decals[] = {
        {.blas = decal, .translation = {10, 10, 10}, .id = 100, .mask = 2},
        {.blas = decal, .translation = {10.3f, 10, 10}, .id = 101, .mask = 2},
        {.blas = decal, .translation = {20, 20, 20}, .id = 102, .mask = 2},
        {.blas = decal, .translation = {10, 10, 10}, .id = 103, .mask = 1},
    };
    s.decals = s.pool.tlas(decals, {{9.5f, 9.5f, 9.5f}, {20.5f, 20.5f, 20.5f}}, 2);
    return s;
}

inline sgl::check::driver_bindings bindings_of(scenes const& s)
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
                {.name = "flipped", .acceleration_root = s.flipped},
                {.name = "flipped_mixed", .acceleration_root = s.flipped_mixed},
                {.name = "moving", .acceleration_root = s.moving},
                {.name = "morphing", .acceleration_root = s.morphing},
                {.name = "morphing_moved", .acceleration_root = s.morphing_moved},
                {.name = "morphing_mixed", .acceleration_root = s.morphing_mixed},
                {.name = "sheet", .acceleration_root = s.sheet},
                {.name = "sheet_flipped", .acceleration_root = s.sheet_flipped},
                {.name = "abutting", .acceleration_root = s.abutting},
                {.name = "tiny", .acceleration_root = s.tiny},
                {.name = "decals", .acceleration_root = s.decals},
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
                                           "    flipped: acceleration_structure[.triangles]\n"
                                           "    flipped_mixed: acceleration_structure[.mixed]\n"
                                           "    moving: acceleration_structure[.triangles]\n"
                                           "    morphing: acceleration_structure[.triangles]\n"
                                           "    morphing_moved: acceleration_structure[.triangles]\n"
                                           "    morphing_mixed: acceleration_structure[.mixed]\n"
                                           "    sheet: acceleration_structure[.triangles]\n"
                                           "    sheet_flipped: acceleration_structure[.triangles]\n"
                                           "    abutting: acceleration_structure[.triangles]\n"
                                           "    tiny: acceleration_structure[.triangles]\n"
                                           "    decals: acceleration_structure[.procedural]\n"
                                           "fun is_near(a: float, b: float) -> bool => abs(a - b) < 1.0e-5\n"
                                           "fun along_z(x: float, y: float) -> ray => ray(origin = pos3(x, y, -1.0), "
                                           "direction = vec3(0.0, 0.0, 1.0))\n";

/// Every failing test of `tests`, run behind the declarations against the scenes.
inline cc::string failures_of(cc::string_view tests, i64 fuel = 1'000'000)
{
    auto source = cc::string(k_declarations);
    source += tests;
    return driven_failures_of(source, bindings_of(build_scenes()), {.fuel = fuel});
}

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
inline cc::string mixed_failures_of(cc::string_view tests)
{
    auto source = cc::string(k_ball);
    source += tests;
    return failures_of(source);
}
} // namespace sgl_test::emulated_scenes
