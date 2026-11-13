#include "driver-test-support.hh"

using namespace sgl_test;

// The emulated trace run on the interpreter, against pools written by hand as the internal doc libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md lays them out.
// Every expectation is worked out from the geometry by hand.

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
    /// Where the translation has moved to at time 1, which makes the instance a moving one (EVAL-130).
    cc::optional<float3> end_translation = {};
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

// Two triangles over the unit square's lower left, primitive 0 at z = 5 and primitive 1 nearer at z = 2.
// Along +z each is a back face: its counterclockwise side faces -z, away from the ray, and a front face turns that side to it.
// That is how dx12 and vulkan hardware report it: DXR's "clockwise from the ray's origin" is stated in a left-handed space.
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
};

/// A bumpy sheet over the 4 by 4 cells from (0, 0): a height on each corner, so no two neighbours share a plane.
/// Its diagonals alternate, and with `is_flipped` every other triangle winds the other way.
/// So a corner of it is met by four triangles or by eight, and an edge by two, of either winding.
cc::vector<triangle> sheet_triangles(bool is_flipped)
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
                                           "fun is_near(a: float, b: float) -> bool => abs(a - b) < 1.0e-5\n"
                                           "fun along_z(x: float, y: float) -> ray => ray(origin = pos3(x, y, -1.0), "
                                           "direction = vec3(0.0, 0.0, 1.0))\n";

/// Every failing test of `tests`, run behind the declarations against the scenes.
cc::string failures_of(cc::string_view tests, i64 fuel = 1'000'000)
{
    auto source = cc::string(k_declarations);
    source += tests;
    return driven_failures_of(source, bindings_of(build_scenes()), {.fuel = fuel});
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
                      "    not h.is_front_face\n"
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

// How many of the sheet's triangles a ray meets: 0 or 1, or 2 where a second one takes it too.
// A slant `from` reaches its target at t = 1 and stops there, since a bump could stand before it.
constexpr cc::string_view k_sheet_counts
    = "fun other(c: triangle_candidate, primitive: int) -> hit_decision:\n"
      "    if c.primitive_index == primitive => return hit_decision.ignore\n"
      "    return hit_decision.accept\n"
      "fun meets(r: ray){scenes} -> int:\n"
      "    let h = scenes.sheet.trace(r, c => other(c, -1), flags = ray_flags.force_non_opaque)\n"
      "    if not h.is_hit => return 0\n"
      "    if scenes.sheet.trace(r, c => other(c, h.primitive_index), flags = ray_flags.force_non_opaque).is_hit => "
      "return "
      "2\n"
      "    return 1\n"
      "fun meets_flipped(r: ray){scenes} -> int:\n"
      "    let h = scenes.sheet_flipped.trace(r, c => other(c, -1), flags = ray_flags.force_non_opaque)\n"
      "    if not h.is_hit => return 0\n"
      "    if scenes.sheet_flipped.trace(r, c => other(c, h.primitive_index), flags = "
      "ray_flags.force_non_opaque).is_hit => return 2\n"
      "    return 1\n"
      "fun from(o: float3, target: float3) -> ray:\n"
      "    let d = target - o\n"
      "    return ray(origin = pos3(o.x, o.y, o.z), direction = vec3(d.x, d.y, d.z), t_min = 0.999, t_max = 1.001)\n"
      "fun height(x: int, y: int) -> float => 2.0 + (((x * 7 + y * 3) % 5) as float) * 0.25\n"
      "fun corner(x: int, y: int) -> float3 => float3(x as float, y as float, height(x, y))\n";

TEST("sgl emulated trace - a ray through an edge two triangles share meets exactly one of them")
{
    auto source = cc::string(k_sheet_counts);
    // the midpoint of every inner edge along x and along y, along +z, along -z, and from a slant
    source += "test {scenes}:\n"
              "    for y in 1 ..= 3:\n"
              "        for x in 0 ..= 3:\n"
              "            let m = (corner(x, y) + corner(x + 1, y)) * 0.5\n"
              "            meets(along_z(m.x, m.y)) == 1\n"
              "            meets_flipped(along_z(m.x, m.y)) == 1\n"
              "            meets(ray(origin = pos3(m.x, m.y, 9.0), direction = vec3(0.0, 0.0, -1.0))) == 1\n"
              "            meets(from(float3(-3.25, -2.5, -7.0), m)) == 1\n"
              "            meets_flipped(from(float3(7.5, 11.0, 13.25), m)) == 1\n"
              "test {scenes}:\n"
              "    for y in 0 ..= 3:\n"
              "        for x in 1 ..= 3:\n"
              "            let m = (corner(x, y) + corner(x, y + 1)) * 0.5\n"
              "            meets(along_z(m.x, m.y)) == 1\n"
              "            meets_flipped(along_z(m.x, m.y)) == 1\n"
              "            meets(from(float3(-3.25, -2.5, -7.0), m)) == 1\n"
              // each cell's diagonal, through its centre
              "test {scenes}:\n"
              "    for y in 0 ..= 3:\n"
              "        for x in 0 ..= 3:\n"
              "            meets(along_z((x as float) + 0.5, (y as float) + 0.5)) == 1\n"
              "            meets_flipped(along_z((x as float) + 0.5, (y as float) + 0.5)) == 1\n";
    CHECK(failures_of(source, 64'000'000) == "");
}

TEST("sgl emulated trace - a ray through a corner a fan of triangles shares meets exactly one of them")
{
    auto source = cc::string(k_sheet_counts);
    source
        += "test {scenes}:\n"
           "    for y in 1 ..= 3:\n"
           "        for x in 1 ..= 3:\n"
           "            meets(along_z(x as float, y as float)) == 1\n"
           "            meets_flipped(along_z(x as float, y as float)) == 1\n"
           "            meets(ray(origin = pos3(x as float, y as float, 9.0), direction = vec3(0.0, 0.0, -1.0))) == 1\n"
           "            meets(from(float3(-3.25, -2.5, -7.0), corner(x, y))) == 1\n"
           "            meets_flipped(from(float3(7.5, 11.0, 13.25), corner(x, y))) == 1\n"
           "            meets(from(float3(2.0, 2.0, -5.0), corner(x, y))) == 1\n";
    CHECK(failures_of(source, 64'000'000) == "");
}

TEST("sgl emulated trace - a ray along the side of a box enters it, and the shared edge is met once")
{
    // x = 1 is the side of both structures' boxes, and d.x is 0, which a slab test's infinity would make a NaN of
    CHECK(failures_of("test {scenes}:\n"
                      "    let h = scenes.abutting.trace(along_z(1.0, 0.25))\n"
                      "    h.is_hit\n"
                      "    is_near(h.t, 3.0)\n"
                      "    is_near(h.barycentrics.x + h.barycentrics.y + h.barycentrics.z, 1.0)\n")
          == "");
}

TEST("sgl emulated trace - a triangle far smaller than the old determinant cutoff is still met")
{
    // its determinant is 1e-14, which the test this one replaced took for edge-on
    CHECK(failures_of("test {scenes}:\n"
                      "    let h = scenes.tiny.trace(along_z(0.25e-7, 0.25e-7))\n"
                      "    h.is_hit\n"
                      "    is_near(h.t, 3.0)\n"
                      "    is_near(h.barycentrics.y, 0.25) and is_near(h.barycentrics.z, 0.25)\n"
                      "    not scenes.tiny.trace(along_z(1.0e-6, 1.0e-6)).is_hit\n")
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

TEST("sgl emulated trace - an instance culling front faces flips its winding")
{
    // the pair turns its back to the ray, so under cull mode front its triangles are front faces: culling those drops both
    CHECK(failures_of("test {scenes}:\n"
                      "    let h = scenes.flipped.trace(along_z(0.25, 0.25))\n"
                      "    h.is_hit and h.primitive_index == 1 and h.instance_id == 17\n"
                      "    h.is_front_face\n"
                      "    not scenes.flipped.trace(along_z(0.25, 0.25), flags = ray_flags.cull_front_facing).is_hit\n"
                      "    scenes.flipped.trace(along_z(0.25, 0.25), flags = ray_flags.cull_back_facing).is_hit\n"
                      // the default mode keeps the winding: the same ray culls the other way round there
                      "    not scenes.pair.trace(along_z(0.25, 0.25), flags = ray_flags.cull_back_facing).is_hit\n")
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
                            "    not h.is_front_face\n"
                            "    let t = h.triangle()\n"
                            "    t.is_hit and is_near(t.t, 3.0) and t.primitive_index == 1\n"
                            "    not h.procedural().is_hit\n")
          == "");
}

TEST("sgl emulated trace - a mixed trace flips the winding of an instance culling front faces")
{
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let h = scenes.flipped_mixed.trace(along_z(0.25, 0.25), ball_report)\n"
                            "    h.kind == hit_kind.triangle and h.instance_id == 17\n"
                            "    h.is_front_face\n"
                            "    not scenes.flipped_mixed.trace(along_z(0.25, 0.25), ball_report, flags = "
                            "ray_flags.cull_front_facing).is_hit\n"
                            "    scenes.flipped_mixed.trace(along_z(0.25, 0.25), ball_report, flags = "
                            "ray_flags.cull_back_facing).is_hit\n")
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

TEST("sgl emulated trace - a trace skipping all its structure holds meets nothing, and a box has no facing")
{
    // the target forbids both skip flags together, so neither form traces at all
    CHECK(mixed_failures_of(
              "test {scenes}:\n"
              "    let h = scenes.pair.trace(along_z(0.25, 0.25), flags = ray_flags.skip_triangles)\n"
              "    h.t == 3.0e38 and not h.is_hit\n"
              "    not scenes.ball.trace(along_z(2.75, 0.75), ball_report, flags = "
              "ray_flags.skip_procedural).is_hit\n"
              "    let both = ray_flags.skip_triangles | ray_flags.skip_procedural\n"
              "    scenes.mixed.trace(along_z(2.75, 0.75), ball_report, flags = both).kind == hit_kind.none\n"
              // a box has no facing, so a trace of boxes that culls by it still meets the ball
              "    scenes.ball.trace(along_z(2.75, 0.75), ball_report, flags = "
              "ray_flags.cull_back_facing).is_hit\n"
              "    scenes.mixed.trace(along_z(2.75, 0.75), ball_report, flags = ray_flags.skip_triangles | "
              "ray_flags.cull_front_facing).kind == hit_kind.procedural\n")
          == "");
}

TEST("sgl emulated trace - a moving instance stands where its ray's time puts it, and its hit says so (EVAL-130)")
{
    // the pair's near triangle is at z = 2, t = 3 from z = -1; at time 0.5 it stands at x = 5
    CHECK(failures_of("fun at(x: float, y: float, time: float) -> ray => ray(origin = pos3(x, y, -1.0), direction = "
                      "vec3(0.0, 0.0, 1.0), time = time)\n"
                      "test {scenes}:\n"
                      "    let h = scenes.moving.trace(at(0.25, 0.25, 0.0))\n"
                      "    h.is_hit and h.instance_id == 21 and is_near(h.t, 3.0)\n"
                      "    not scenes.moving.trace(at(0.25, 0.25, 1.0)).is_hit\n"
                      "    scenes.moving.trace(at(10.25, 0.25, 1.0)).instance_id == 21\n"
                      "    not scenes.moving.trace(at(10.25, 0.25, 0.0)).is_hit\n"
                      "    let m = scenes.moving.trace(at(5.25, 0.25, 0.5))\n"
                      "    m.is_hit and is_near(m.barycentrics.y, 0.25) and is_near(m.barycentrics.z, 0.25)\n"
                      "    let p = m.to_world(pos3(0.0, 0.0, 2.0))\n"
                      "    is_near(p.x, 5.0) and is_near(p.y, 0.0) and is_near(p.z, 2.0)\n"
                      "    let q = m.to_object(pos3(5.25, 0.25, 2.0))\n"
                      "    is_near(q.x, 0.25) and is_near(q.y, 0.25) and is_near(q.z, 2.0)\n"
                      // a time outside 0 to 1 is held at its end, and a ray with none is cast at 0
                      "test {scenes}:\n"
                      "    scenes.moving.trace(at(0.25, 0.25, -3.0)).instance_id == 21\n"
                      "    scenes.moving.trace(at(10.25, 0.25, 7.0)).instance_id == 21\n"
                      "    scenes.moving.trace(along_z(0.25, 0.25)).instance_id == 21\n"
                      // the static instance beside it stands still at every time, in a TLAS of 10-unit instances
                      "test {scenes}:\n"
                      "    scenes.moving.trace(at(0.25, 50.25, 0.0)).instance_id == 22\n"
                      "    scenes.moving.trace(at(0.25, 50.25, 1.0)).instance_id == 22\n")
          == "");
}

TEST("sgl emulated trace - a moving triangle stands where its ray's time puts it, each vertex on its own (EVAL-132)")
{
    // z falls from 5 to 1, so a ray from z = -1 meets it at t = 6 - 4 * time
    CHECK(failures_of("fun at(x: float, y: float, time: float) -> ray => ray(origin = pos3(x, y, -1.0), direction = "
                      "vec3(0.0, 0.0, 1.0), time = time)\n"
                      "test {scenes}:\n"
                      "    is_near(scenes.morphing.trace(at(0.25, 0.25, 0.0)).t, 6.0)\n"
                      "    is_near(scenes.morphing.trace(at(0.25, 0.25, 1.0)).t, 2.0)\n"
                      "    is_near(scenes.morphing.trace(at(0.25, 0.25, 0.25)).t, 5.0)\n"
                      // a time outside 0 to 1 is held at its end, and a ray with none is cast at 0
                      "test {scenes}:\n"
                      "    is_near(scenes.morphing.trace(at(0.25, 0.25, -3.0)).t, 6.0)\n"
                      "    is_near(scenes.morphing.trace(at(0.25, 0.25, 7.0)).t, 2.0)\n"
                      "    is_near(scenes.morphing.trace(along_z(0.25, 0.25)).t, 6.0)\n"
                      // the vertices move apart: x = 1.5 is past the edge at time 0 and inside it at time 1
                      "test {scenes}:\n"
                      "    not scenes.morphing.trace(at(1.5, 0.1, 0.0)).is_hit\n"
                      "    scenes.morphing.trace(at(1.5, 0.1, 1.0)).is_hit\n"
                      // at time 0.5 the second vertex is at x = 2, and barycentrics are of the triangle at that time
                      "test {scenes}:\n"
                      "    let m = scenes.morphing.trace(at(1.0, 0.25, 0.5))\n"
                      "    m.is_hit and m.primitive_index == 0 and m.geometry_index == 0\n"
                      "    is_near(m.t, 4.0) and is_near(m.barycentrics.y, 0.5) and is_near(m.barycentrics.z, 0.25)\n"
                      // the still triangle of the same structure stands at every time
                      "test {scenes}:\n"
                      "    let s = scenes.morphing.trace(at(20.25, 0.25, 0.75))\n"
                      "    s.primitive_index == 1 and is_near(s.t, 6.0)\n")
          == "");
}

TEST("sgl emulated trace - moving vertices in a moving instance take the same time, on the mixed walk too")
{
    CHECK(mixed_failures_of("fun at(x: float, y: float, time: float) -> ray => ray(origin = pos3(x, y, -1.0), "
                            "direction = "
                            "vec3(0.0, 0.0, 1.0), time = time)\n"
                            // at time 0.5 the instance stands at x = 5 and the triangle at z = 3
                            "test {scenes}:\n"
                            "    let h = scenes.morphing_moved.trace(at(5.25, 0.25, 0.5))\n"
                            "    h.instance_id == 32 and is_near(h.t, 4.0)\n"
                            "    not scenes.morphing_moved.trace(at(0.25, 0.25, 0.5)).is_hit\n"
                            "    let p = h.to_world(pos3(0.0, 0.0, 3.0))\n"
                            "    is_near(p.x, 5.0) and is_near(p.z, 3.0)\n"
                            "test {scenes}:\n"
                            "    let h = scenes.morphing_mixed.trace(at(0.25, 0.25, 1.0), ball_report)\n"
                            "    h.kind == hit_kind.triangle and is_near(h.t, 2.0)\n"
                            "    let i = scenes.morphing_mixed.trace(at(0.25, 0.25, 0.0), ball_report)\n"
                            "    i.kind == hit_kind.triangle and is_near(i.t, 6.0)\n")
          == "");
}
