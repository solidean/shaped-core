#include "emulated-scenes.hh"

using namespace sgl_test;
using namespace sgl_test::emulated_scenes;

// The emulated trace run on the interpreter, against pools written by hand as the internal doc libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md lays them out.
// Every expectation is worked out from the geometry by hand.

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

TEST("sgl emulated trace - a bounding run takes a trace exactly, as a device's own trace is its own precision")
{
    auto output = cc::pinned_data<byte>::create_filled(8, byte(0));
    auto bounds = cc::pinned_data<f32>::create_filled(4, cc::bit_cast<f32>(0x7fc0'0000u));
    auto bindings = bindings_of(build_scenes());
    bindings.groups.push_back({.name = "io",
                               .members = {{.name = "input", .bytes = floats(0.3f, 0.7f)},
                                           {.name = "output", .mutable_bytes = output, .bounds = bounds}}});
    auto source = cc::string(k_declarations);
    // 0.3 * 0.7 is no float, so a contraction may keep it unrounded, and the ray's x is ranged
    source += "binding io:\n    input: buffer[float]\n    output: mut buffer[float]\n"
              "test {scenes, io}:\n"
              "    let x = io.input[0] * io.input[1]\n"
              "    let h = scenes.pair.trace(along_z(x, 0.25))\n"
              "    io.output[0] = x\n"
              "    io.output[1] = h.t\n"
              "    h.is_hit\n";
    CHECK(driven_failures_of(source, bindings, {.bounds_floats = true}) == "");
    CHECK(bounds[0] < bounds[1]);
    CHECK(float_at(output, 1) == 3.0f);
    CHECK(bounds[2] != bounds[2]);
}
