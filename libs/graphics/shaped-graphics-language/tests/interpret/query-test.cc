#include "emulated-scenes.hh"

using namespace sgl_test;
using namespace sgl_test::emulated_scenes;

// The candidate loop's emulated form on the interpreter (EMIT-277, EVAL-95): a query steps through the walk of the
// pools emulated-scenes.hh writes, and its answers are held to the hand-worked geometry and to `world.trace`'s.

TEST("sgl query - a query of the empty structure meets no candidate, and its hit is the ray's end")
{
    CHECK(failures_of("test {scenes}:\n"
                      "    let q = scenes.empty.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "    n == 0\n"
                      "    not q.hit.is_hit\n"
                      "    q.hit.t == 3.0e38\n")
          == "");
}

TEST("sgl query - an opaque triangle is committed by the traversal, and no body is asked about it")
{
    // CHK-557: a candidate is what the traversal cannot decide alone
    CHECK(failures_of("test {scenes}:\n"
                      "    let q = scenes.pair.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "    n == 0\n"
                      "    let h = q.hit\n"
                      "    h.is_hit and is_near(h.t, 3.0) and h.primitive_index == 1 and h.instance_id == 7\n"
                      "    is_near(h.barycentrics.y, 0.25) and is_near(h.barycentrics.z, 0.25)\n")
          == "");
}

TEST("sgl query - a non-opaque triangle is a candidate, and only a commit makes it the hit")
{
    CHECK(failures_of("test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "    n == 2\n"
                      "    not q.hit.is_hit\n"
                      // the farther triangle alone, then both: the nearer is the hit
                      "test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    for c in q.candidates:\n"
                      "        if c.primitive_index == 0 => c.commit()\n"
                      "    q.hit.is_hit and q.hit.primitive_index == 0 and is_near(q.hit.t, 6.0)\n"
                      "test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    for c in q.candidates:\n"
                      "        c.commit()\n"
                      "    q.hit.is_hit and q.hit.primitive_index == 1 and is_near(q.hit.t, 3.0)\n"
                      "    q.hit.instance_id == 9\n")
          == "");
}

TEST("sgl query - a candidate reads what the traversal stopped at, and t_current what it committed")
{
    // CHK-557
    CHECK(failures_of("test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    let mut seen = 0\n"
                      "    let mut current_after = 0.0\n"
                      "    let mut good = true\n"
                      // a member reads the query's state, which an assert's condition may not call for
                      "    for c in q.candidates:\n"
                      "        good = good and c.is_triangle and not c.is_procedural\n"
                      "        good = good and c.instance_id == 9 and c.instance_index == 0 and c.geometry_index == 0\n"
                      "        good = good and c.t_min == 0.0 and c.ray.direction.z == 1.0\n"
                      "        good = good and c.triangle().t == c.t and c.triangle().primitive_index == "
                      "c.primitive_index\n"
                      "        good = good and is_near(c.barycentrics.y, 0.25) and is_near(c.barycentrics.z, 0.25)\n"
                      "        good = good and c.cluster_id == -1 and not c.is_front_face\n"
                      "        good = good and is_near(c.object_ray.origin.x, 0.25) and c.object_ray.t_max == "
                      "c.t_current\n"
                      "        good = good and c.object_to_world.row(0).x == 1.0 and c.world_to_object.row(2).z == "
                      "1.0\n"
                      "        if seen == 0:\n"
                      "            good = good and c.t_current == 3.0e38\n"
                      "            good = good and c.commit()\n"
                      "            current_after = c.t_current\n"
                      "        seen += 1\n"
                      "    good\n"
                      "    current_after > 0.0 and current_after < 7.0\n"
                      "    q.hit.is_hit\n")
          == "");
}

TEST("sgl query - break keeps what was committed, continue leaves a candidate, and return leaves both")
{
    // CHK-555
    CHECK(failures_of("fun first_primitive(r: ray){scenes} -> int:\n"
                      "    let q = scenes.cutout.query(r)\n"
                      "    for c in q.candidates:\n"
                      "        return c.primitive_index\n"
                      "    return -1\n"
                      "test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "        c.commit()\n"
                      "        break\n"
                      "    n == 1 and q.hit.is_hit\n"
                      "test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "        if c.primitive_index == 1 => continue\n"
                      "        c.commit()\n"
                      "    n == 2 and q.hit.primitive_index == 0\n"
                      "test {scenes}:\n"
                      "    let p = first_primitive(along_z(0.25, 0.25))\n"
                      "    p == 0 or p == 1\n"
                      "    first_primitive(along_z(0.75, 0.75)) == -1\n"
                      // a loop in the body keeps its own break
                      "test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        for k in 0 ..< 4:\n"
                      "            if k == 1 => break\n"
                      "            n += 1\n"
                      "    n == 2\n")
          == "");
}

TEST("sgl query - accept_first_hit_and_end_search ends the traversal at the first commit that commits")
{
    // CHK-558
    CHECK(failures_of("test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25), flags = "
                      "ray_flags.accept_first_hit_and_end_search)\n"
                      "    let mut n = 0\n"
                      "    let mut after = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "        if c.commit() => after += 1\n"
                      "    n == 1 and after == 1 and q.hit.is_hit\n")
          == "");
}

TEST("sgl query - flags and a mask choose what a query meets, as they do a trace's")
{
    // CHK-553
    CHECK(failures_of("test {scenes}:\n"
                      // a query that skips every kind its structure holds meets nothing, opaque or not
                      "    let a = scenes.pair.query(along_z(0.25, 0.25), flags = ray_flags.skip_triangles)\n"
                      "    let mut n = 0\n"
                      "    for c in a.candidates:\n"
                      "        n += 1\n"
                      "    not a.hit.is_hit\n"
                      // forced opaque, the traversal commits every triangle itself
                      "    let b = scenes.cutout.query(along_z(0.25, 0.25), flags = ray_flags.force_opaque)\n"
                      "    for c in b.candidates:\n"
                      "        n += 1\n"
                      "    n == 0 and b.hit.is_hit and is_near(b.hit.t, 3.0)\n"
                      "    let m = scenes.cutout.query(along_z(0.25, 0.25), mask = 0)\n"
                      "    for c in m.candidates:\n"
                      "        n += 1\n"
                      "    n == 0 and not m.hit.is_hit\n"
                      // the facing culls drop a candidate before any body sees it
                      "    let k = scenes.cutout.query(along_z(0.25, 0.25), flags = ray_flags.cull_back_facing)\n"
                      "    for c in k.candidates:\n"
                      "        n += 1\n"
                      "    n == 0\n")
          == "");
}

TEST("sgl query - a box is a candidate, opaque or not, and a commit within the ray's range is its hit")
{
    // CHK-557, CHK-558: a box with no commit is no hit; a commit outside t_min to t_current does nothing
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let q = scenes.ball.query(along_z(2.75, 0.75))\n"
                            "    let mut n = 0\n"
                            "    let mut good = true\n"
                            "    for c in q.candidates:\n"
                            "        n += 1\n"
                            "        good = good and c.is_procedural and c.instance_id == 11 and not c.is_opaque\n"
                            "        let b = c.box()\n"
                            "        good = good and b.t_current == 3.0e38 and b.primitive_index == 0\n"
                            "        good = good and is_near(b.object_ray.origin.x, 2.75)\n"
                            "    good\n"
                            "    n == 1 and not q.hit.is_hit\n"
                            "test {scenes}:\n"
                            "    let q = scenes.ball.query(along_z(2.75, 0.75))\n"
                            "    let mut committed = 0\n"
                            "    let mut good = true\n"
                            "    for c in q.candidates:\n"
                            "        let reported = ball_report(c.box())\n"
                            "        let early = c.commit(-1.0)\n"
                            "        if reported.is_hit and c.commit(reported.t) and not early => committed += 1\n"
                            "        good = good and not c.commit(reported.t + 1.0)\n"
                            "        good = good and c.t_current == reported.t\n"
                            "    good\n"
                            "    committed == 1\n"
                            "    q.hit.is_hit and is_near(q.hit.t, 2.0 - sqrt(0.875)) and q.hit.instance_id == 11\n"
                            "    q.hit.attributes == void\n")
          == "");
}

TEST("sgl query - a mixed query meets both kinds, and a commit of the other kind does nothing")
{
    // CHK-557, CHK-558 on `.mixed`: the ball, and behind it instance 13's two triangles, forced non-opaque
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let all = scenes.mixed.query(along_z(2.75, 0.75))\n"
                            "    let mut triangles = 0\n"
                            "    let mut boxes = 0\n"
                            "    for c in all.candidates:\n"
                            "        if c.is_triangle:\n"
                            "            triangles += 1\n"
                            "        else:\n"
                            "            boxes += 1\n"
                            "    triangles == 2 and boxes == 1 and all.hit.kind == hit_kind.none\n"
                            "test {scenes}:\n"
                            "    let q = scenes.mixed.query(along_z(2.75, 0.75))\n"
                            "    let mut boxes = 0\n"
                            "    let mut good = true\n"
                            "    for c in q.candidates:\n"
                            "        if c.is_triangle:\n"
                            "            good = good and not c.commit(c.t)\n"
                            "        else:\n"
                            "            boxes += 1\n"
                            "            good = good and not c.commit()\n"
                            "            let reported = ball_report(c.box())\n"
                            "            good = good and reported.is_hit and c.commit(reported.t)\n"
                            "    good and boxes == 1\n"
                            "    q.hit.kind == hit_kind.procedural and q.hit.instance_id == 11\n")
          == "");
}

TEST("sgl query - a trace's decisions written as a candidate loop give what the trace gives")
{
    // CHK-560: `world.trace` is the candidate loop
    CHECK(mixed_failures_of("fun looped(r: ray){scenes} -> triangle_hit:\n"
                            "    let q = scenes.cutout.query(r)\n"
                            "    for c in q.candidates:\n"
                            "        let d = cut_near(c.triangle())\n"
                            "        if d != hit_decision.ignore => c.commit()\n"
                            "        if d == hit_decision.accept_and_end_search => break\n"
                            "    return q.hit\n"
                            "fun looped_ball(r: ray){scenes} -> procedural_hit[void]:\n"
                            "    let q = scenes.ball.query(r)\n"
                            "    for c in q.candidates:\n"
                            "        let reported = ball_report(c.box())\n"
                            "        if reported.is_hit:\n"
                            "            let _ = c.commit(reported.t)\n"
                            "    return q.hit\n"
                            "test {scenes}:\n"
                            "    for k in 0 ..< 4:\n"
                            "        let r = along_z(0.25 + 0.5 * (k as float), 0.25)\n"
                            "        let a = looped(r)\n"
                            "        let b = scenes.cutout.trace(r, cut_near)\n"
                            "        a.is_hit == b.is_hit and a.t == b.t and a.primitive_index == b.primitive_index\n"
                            "        let s = along_z(2.25 + 0.5 * (k as float), 0.75)\n"
                            "        let p = looped_ball(s)\n"
                            "        let t = scenes.ball.trace(s, ball_report)\n"
                            "        p.is_hit == t.is_hit and p.t == t.t\n")
          == "");
}

TEST("sgl query - a query in a candidate loop's body is a trace of its own")
{
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                            "    let mut inner_hits = 0\n"
                            "    for c in q.candidates:\n"
                            "        let inner = scenes.cutout.query(along_z(0.25, 0.25))\n"
                            "        for d in inner.candidates:\n"
                            "            d.commit()\n"
                            "        if inner.hit.is_hit => inner_hits += 1\n"
                            "        if inner_hits == 2 => c.commit()\n"
                            "    inner_hits == 2 and q.hit.is_hit\n")
          == "");
}

TEST("sgl query - the decal lookup collects every decal a point lies in, by the instance's mask")
{
    // the example of raytracing.md's "The decal lookup", on the interpreter
    CHECK(failures_of("fun gathered(p: float3){scenes} -> int4:\n"
                      "    let r = ray(origin = pos3(p.x, p.y, p.z), direction = vec3(0.0, 0.0, 1.0), t_max = 1.0e-4)\n"
                      "    @indexed(.memory) let mut ids = int[8].filled(-1)\n"
                      "    let mut count = 0\n"
                      "    let q = scenes.decals.query(r, mask = 0x02)\n"
                      "    for c in q.candidates:\n"
                      "        let o = c.object_ray.origin\n"
                      "        if abs(o.x) <= 0.5 and abs(o.y) <= 0.5 and abs(o.z) <= 0.5:\n"
                      "            ids[count] = c.instance_id\n"
                      "            count += 1\n"
                      "            if count == 8 => break\n"
                      "    return int4(count, min(ids[0], ids[1]), max(ids[0], ids[1]), ids[2])\n"
                      "test {scenes}:\n"
                      "    gathered(float3(10.1, 10.0, 10.0)) == int4(2, 100, 101, -1)\n"
                      "    gathered(float3(9.7, 10.0, 10.0)) == int4(1, -1, 100, -1)\n"
                      "    gathered(float3(20.0, 20.2, 19.9)) == int4(1, -1, 102, -1)\n"
                      "    gathered(float3(15.0, 15.0, 15.0)) == int4(0, -1, -1, -1)\n")
          == "");
}

TEST("sgl query - a box's commit takes t_min and t_current themselves, and a NaN commits nothing")
{
    // CHK-558: `t_min <= t <= t_current`, both ends included; a NaN is in no range
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let q = scenes.ball.query(along_z(2.75, 0.75))\n"
                            "    let mut good = true\n"
                            "    for c in q.candidates:\n"
                            "        let zero = c.t_min\n"
                            "        let nan = zero / zero\n"
                            "        good = good and not c.commit(nan)\n"
                            "        good = good and c.commit(c.t_current)\n"
                            "        good = good and c.t_current == 3.0e38\n"
                            "        good = good and c.commit(1.5)\n"
                            "        good = good and not c.commit(1.75)\n"
                            "        good = good and c.commit(c.t_current) and c.t_current == 1.5\n"
                            "        good = good and c.commit(c.t_min) and c.t_current == 0.0\n"
                            "    good\n"
                            "    q.hit.is_hit and q.hit.t == 0.0\n")
          == "");
}

TEST("sgl query - under accept_first_hit_and_end_search the rest of a committing body runs, then the walk ends")
{
    // CHK-558: a commit that commits ends the traversal once the body is done, a `continue` after it too
    CHECK(failures_of("test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25), flags = "
                      "ray_flags.accept_first_hit_and_end_search)\n"
                      "    let mut n = 0\n"
                      "    let mut after = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "        c.commit()\n"
                      "        after += 1\n"
                      "        if after > 0 => continue\n"
                      "        after += 100\n"
                      "    n == 1 and after == 1 and q.hit.is_hit\n")
          == "");
}

TEST("sgl query - a mixed query that skips triangles meets its boxes alone")
{
    // CHK-553: `skip_triangles` on `.mixed` leaves the boxes; the ball is the one candidate on this ray
    CHECK(mixed_failures_of("test {scenes}:\n"
                            "    let q = scenes.mixed.query(along_z(2.75, 0.75), flags = ray_flags.skip_triangles)\n"
                            "    let mut triangles = 0\n"
                            "    let mut boxes = 0\n"
                            "    for c in q.candidates:\n"
                            "        if c.is_triangle:\n"
                            "            triangles += 1\n"
                            "        else:\n"
                            "            boxes += 1\n"
                            "            let reported = ball_report(c.box())\n"
                            "            if reported.is_hit => c.commit(reported.t)\n"
                            "    triangles == 0 and boxes == 1\n"
                            "    q.hit.kind == hit_kind.procedural and q.hit.instance_id == 11\n")
          == "");
}

TEST("sgl query - early exits through loops in the body, a helper and a candidate handed on keep the walk right")
{
    // CHK-554, CHK-555: a return from a loop inside the body leaves the traversal and the function
    CHECK(failures_of("fun deep(r: ray){scenes} -> int:\n"
                      "    let q = scenes.cutout.query(r)\n"
                      "    for c in q.candidates:\n"
                      "        let mut i = 0\n"
                      "        while i < 4:\n"
                      "            if c.primitive_index == i => return 10 + c.primitive_index\n"
                      "            i += 1\n"
                      "    return -1\n"
                      "fun take(c: query_candidate[.triangles]) -> bool => c.commit()\n"
                      "fun count_of(q: trace_query[.triangles]) -> int => q.hit.primitive_index\n"
                      "test {scenes}:\n"
                      "    let mut total = 0\n"
                      "    for k in 0 ..< 3:\n"
                      "        let d = deep(along_z(0.25, 0.25))\n"
                      "        total += if d == 10 or d == 11 => 1 else 0\n"
                      "    total == 3\n"
                      "    deep(along_z(0.75, 0.75)) == -1\n"
                      "test {scenes}:\n"
                      "    let q = scenes.cutout.query(along_z(0.25, 0.25))\n"
                      "    let mut n = 0\n"
                      "    for c in q.candidates:\n"
                      "        n += 1\n"
                      "        loop:\n"
                      "            if c.primitive_index == 0 and take(c) => break\n"
                      "            break\n"
                      "    n == 2 and q.hit.is_hit and count_of(q) == 0\n")
          == "");
}
