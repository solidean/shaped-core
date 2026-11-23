#include "check-test-support.hh"

using namespace sgl_test;

// The candidate loop's rules (CHK-540 to CHK-546): what the checker refuses of a query and its candidates, from the text
// and, for a query a function is handed, from the inlined tree.
// What a query does when it runs is the corpus's (tests/corpus/types/queries.sgl) and the interpreter's
// (tests/interpret/query-test.cc).

namespace
{
constexpr auto scene = cc::string_view("require ray_query\n"
                                       "binding scene:\n"
                                       "    world: acceleration_structure[.triangles]\n"
                                       "    boxes: acceleration_structure[.procedural]\n"
                                       "    mixed: acceleration_structure[.mixed]\n"
                                       "    hits: mut buffer[float4]\n"
                                       "fun probe() -> ray => ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, "
                                       "0.0, 1.0))\n");

/// What the check reports of `declarations` and a compute entry point whose body is `body`, indented by the caller.
cc::string reports_of_body(cc::string_view declarations, cc::string_view body)
{
    auto source = cc::string(scene);
    source += declarations;
    source += "@compute(64) fun main_cs(@thread_id id: int3){scene}:\n";
    auto is_line_start = true;
    for (auto const ch : body)
    {
        if (is_line_start)
            source += "    ";
        source += ch;
        is_line_start = ch == '\n';
    }
    return reports_for(source);
}

void refused(cc::string_view declarations, cc::string_view body, cc::string_view kind, cc::string_view detail = {})
{
    auto const report = reports_of_body(declarations, body);
    CHECK(report.contains(kind)).dump("report", report).dump("body", body);
    if (!detail.empty())
        CHECK(report.contains(detail)).dump("report", report).dump("body", body);
}

void accepted(cc::string_view declarations, cc::string_view body)
{
    auto const report = reports_of_body(declarations, body);
    CHECK(report == "").dump("report", report).dump("body", body);
}
} // namespace

TEST("sgl check - a query is a let of world.query, stepped through by one loop, its hit read after it")
{
    // CHK-540, CHK-542, CHK-546: the loop in the let's block, the hit after it, any number of times
    accepted("", "let q = scene.world.query(probe())\n"
                 "for c in q.candidates:\n"
                 "    if c.t < 4.0 => c.commit()\n"
                 "let h = q.hit\n"
                 "if h.is_hit => scene.hits[id.x] = float4(h.t, q.hit.t, 0.0, 0.0)\n");
    // flags by name, a constant, and a mask that is any int
    accepted("", "let q = scene.boxes.query(probe(), mask = id.x & 0xff, flags = ray_flags.force_opaque | "
                 "ray_flags.cull_opaque)\n"
                 "for c in q.candidates:\n"
                 "    let _ = c.commit(c.t_min)\n"
                 "scene.hits[id.x] = float4(q.hit.t)\n");
    // a query in a loop is a new trace each time round, and the hit is read in a block within the loop's
    accepted("", "for i in 0 ..< 2:\n"
                 "    let q = scene.mixed.query(probe())\n"
                 "    for c in q.candidates:\n"
                 "        if c.is_triangle => continue\n"
                 "        break\n"
                 "    if i == 1:\n"
                 "        scene.hits[id.x] = float4(q.hit.t)\n");
    // a query inside a candidate loop's body is a trace of its own
    accepted("", "let q = scene.mixed.query(probe())\n"
                 "for c in q.candidates:\n"
                 "    let inner = scene.world.query(c.ray)\n"
                 "    for d in inner.candidates:\n"
                 "        d.commit()\n"
                 "    if inner.hit.is_hit => break\n"
                 "scene.hits[id.x] = float4(q.hit.t)\n");
}

TEST("sgl check - a query's flags are a constant, and its arguments those of a trace")
{
    // CHK-540
    refused("",
            "let f = if id.x > 0 => ray_flags.force_opaque else ray_flags.none\n"
            "let q = scene.world.query(probe(), flags = f)\n"
            "for c in q.candidates:\n    c.commit()\n",
            "invalid-constant-argument", "a query's flags are a constant");
    refused("", "let q = scene.world.query(probe(), 3)\nfor c in q.candidates:\n    c.commit()\n",
            "no-matching-overload");
    refused("", "let q = scene.world.query(probe(), mask = 1.0)\nfor c in q.candidates:\n    c.commit()\n",
            "no-matching-overload");
    refused("", "let q = scene.world.query(probe(), time = 1.0)\nfor c in q.candidates:\n    c.commit()\n",
            "no-matching-overload");
}

TEST("sgl check - a query and a candidate stay where their let and their loop put them")
{
    // CHK-541
    refused("", "let mut q = scene.world.query(probe())\n", "query-escapes", "`let mut` of a query");
    refused("", "let q = scene.world.query(probe())\nlet p = q\n", "query-escapes");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let d = c\n", "query-escapes",
            "the variable of its loop");
    refused("", "let q = scene.world.query(probe())\nprint q\n", "query-escapes", "a print records values");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    print \"at $c\"\n", "query-escapes");
    // through what holds a value, which only the inlined tree shows
    refused("struct held:\n    q: trace_query[.triangles]\n",
            "let q = scene.world.query(probe())\nlet h = held(q)\nfor c in q.candidates:\n    c.commit()\n",
            "query-escapes", "no other local, field, element");
    refused("fun same(q: trace_query[.triangles]) -> trace_query[.triangles]:\n    return q\n",
            "let q = scene.world.query(probe())\nfor c in q.candidates:\n    c.commit()\n"
            "scene.hits[id.x] = float4(same(q).hit.t)\n",
            "query-escapes");
    refused("fun same[T](x: T) -> T => x\n",
            "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let t = same(c).t\n", "query-escapes");
    refused("",
            "let q = scene.world.query(probe())\nlet r = scene.world.query(probe())\n"
            "for c in (if id.x > 0 => q else r).candidates:\n    c.commit()\n",
            "query-traversal-misplaced");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let pair = [c, c]\n", "query-escapes");
}

TEST("sgl check - a function may be handed a query or a candidate, which stands for the name it was given")
{
    // CHK-541: a parameter stands for its argument
    auto const helpers
        = cc::string_view("fun near(c: query_candidate[.triangles], limit: float) -> bool => c.t < limit\n"
                          "fun take(c: query_candidate[.triangles]):\n    c.commit()\n"
                          "fun nearest(q: trace_query[.triangles]) -> float => q.hit.t\n");
    accepted(helpers, "let q = scene.world.query(probe())\n"
                      "for c in q.candidates:\n"
                      "    if near(c, 5.0) => take(c)\n"
                      "scene.hits[id.x] = float4(nearest(q))\n");
    // CHK-546: a hit read through a parameter is judged where the call is inlined
    refused(helpers,
            "let q = scene.world.query(probe())\n"
            "scene.hits[id.x] = float4(nearest(q))\n"
            "for c in q.candidates:\n    take(c)\n",
            "query-hit-too-early", "none before here");
    refused("fun inside(q: trace_query[.triangles], c: query_candidate[.triangles]) -> float => q.hit.t + c.t\n",
            "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let t = inside(q, c)\n",
            "query-hit-too-early", "stands inside it");
    // CHK-542: a query a function is handed is stepped through where its let stands
    refused("fun step(q: trace_query[.triangles]):\n    for c in q.candidates:\n        c.commit()\n",
            "let q = scene.world.query(probe())\nstep(q)\n", "query-traversal-misplaced", "a query a function is handed");
}

TEST("sgl check - a candidate loop stands in its query's block, once, and nowhere else does its candidates")
{
    // CHK-542
    refused("", "let q = scene.world.query(probe())\nif id.x > 0:\n    for c in q.candidates:\n        c.commit()\n",
            "query-traversal-misplaced", "at its level");
    refused("", "let q = scene.world.query(probe())\nfor i in 0 ..< 2:\n    for c in q.candidates:\n        c.commit()\n",
            "query-traversal-misplaced");
    refused("",
            "let q = scene.world.query(probe())\nfor c in q.candidates:\n    c.commit()\nfor c in q.candidates:\n"
            "    c.commit()\n",
            "query-traversal-misplaced", "one already");
    refused("", "let q = scene.world.query(probe())\nlet all = q.candidates\n", "query-traversal-misplaced",
            "behind the `in`");
    refused("", "let q = scene.world.query(probe())\n@unroll for c in q.candidates:\n    c.commit()\n",
            "unsupported-yet", "candidate loop");
    refused("", "let q = scene.world.query(probe())\n@max_iterations(4) for c in q.candidates:\n    c.commit()\n",
            "unsupported-yet", "candidate loop");
}

TEST("sgl check - a candidate loop's body is a decision, which writes nothing through a binding")
{
    // CHK-543
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    scene.hits[id.x] = float4(c.t)\n",
            "stage-not-allowed", "candidate loop's body");
    // its own locals and the ones around it are its to write
    accepted("", "let q = scene.world.query(probe())\nlet mut n = 0\nfor c in q.candidates:\n    n += 1\n"
                 "scene.hits[id.x] = float4(n as float)\n");
}

TEST("sgl check - a candidate's members are its kind's, and a commit of the other kind is no overload")
{
    // CHK-544
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let o = c.is_opaque\n",
            "unknown-member", "holds triangles");
    refused("", "let q = scene.boxes.query(probe())\nfor c in q.candidates:\n    let t = c.t\n", "unknown-member",
            "only a triangle candidate has");
    refused("", "let q = scene.boxes.query(probe())\nfor c in q.candidates:\n    let t = c.triangle()\n",
            "unknown-member");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let b = c.box()\n", "unknown-member");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let x = c.nothing\n", "unknown-member");
    refused("", "let q = scene.world.query(probe())\nlet x = q.ray\n", "unknown-member");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let b = c.triangle\n",
            "wrong-kind-of-name", "is called");
    // on a mixed structure every member is one
    accepted("", "let q = scene.mixed.query(probe())\nlet mut s = 0.0\n"
                 "for c in q.candidates:\n"
                 "    s += c.t + c.t_min + c.t_current + c.barycentrics.x + c.object_ray.origin.x\n"
                 "    if c.is_opaque and c.is_front_face and c.cluster_id < 0 and c.is_procedural => break\n"
                 "    s += (c.instance_id + c.instance_index + c.geometry_index + c.primitive_index) as float\n"
                 "    s += c.ray.t_max + c.object_to_world.row(0).x + c.world_to_object.row(0).x\n"
                 "    s += c.triangle().t + c.box().t_current\n"
                 "    if c.commit() or c.commit(c.t_min) => continue\n"
                 "scene.hits[id.x] = float4(s, q.hit.t, 0.0, 0.0)\n");
    // CHK-545
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let _ = c.commit(1.0)\n",
            "no-matching-overload", "meets no box");
    refused("", "let q = scene.boxes.query(probe())\nfor c in q.candidates:\n    let _ = c.commit()\n",
            "no-matching-overload", "meets no triangle");
    refused("", "let q = scene.boxes.query(probe())\nfor c in q.candidates:\n    let _ = c.commit(id.x)\n",
            "type-mismatch");
    refused("", "let q = scene.boxes.query(probe())\nfor c in q.candidates:\n    let _ = c.commit(1.0, 2.0)\n",
            "no-matching-overload");
    // a commit gives whether it committed, a bool
    refused("", "let q = scene.boxes.query(probe())\nfor c in q.candidates:\n    let x: float = c.commit(1.0)\n",
            "type-mismatch");
}

TEST("sgl check - a query's hit is read after its loop alone")
{
    // CHK-546
    refused("", "let q = scene.world.query(probe())\nlet h = q.hit\n", "query-hit-too-early", "none before here");
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    let t = q.hit.t\n",
            "query-hit-too-early", "c.t_current");
    refused("", "let q = scene.world.query(probe())\nlet h = q.hit\nfor c in q.candidates:\n    c.commit()\n",
            "query-hit-too-early");
    // a query's hit is the trace's types: a triangle's, and a procedural or mixed one of void attributes
    accepted("fun hits(h: triangle_hit, p: procedural_hit[void], m: mixed_hit[void]) -> float => h.t + p.t + m.t\n",
             "let a = scene.world.query(probe())\nfor c in a.candidates:\n    c.commit()\n"
             "let b = scene.boxes.query(probe())\nfor c in b.candidates:\n    let _ = c.commit(1.0)\n"
             "let m = scene.mixed.query(probe())\nfor c in m.candidates:\n    break\n"
             "scene.hits[id.x] = float4(hits(a.hit, b.hit, m.hit))\n");
}

TEST("sgl check - a query and a candidate are spelled with their structure's geometry")
{
    CHECK(reports_for("fun f(q: trace_query) -> int => 0\n").contains("wrong-kind-of-name"));
    CHECK(reports_for("fun f(c: query_candidate[.spheres]) -> int => 0\n").contains("wrong-kind-of-name"));
    CHECK(reports_for("fun f(c: query_candidate[.procedural]) -> bool => c.is_opaque\n") == "");
}

TEST("sgl check - a candidate loop's body is non-uniform control flow, whatever its query's ray")
{
    // CHK-542: each invocation meets its own candidates, so a barrier or a derivative there is refused even where every
    // invocation traces the same ray
    refused("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    workgroup_barrier()\n    c.commit()\n",
            "non-uniform-control-flow");
    refused("", "let q = scene.world.query(probe(), mask = id.x)\nfor c in q.candidates:\n    workgroup_barrier()\n",
            "non-uniform-control-flow");
}

TEST("sgl check - a mixed query that skips triangles is a query of its boxes")
{
    // CHK-540: the flags a target forbids beside `skip_triangles` are left out, and the boxes are met
    accepted("", "let q = scene.mixed.query(probe(), flags = ray_flags.skip_triangles)\n"
                 "for c in q.candidates:\n"
                 "    let _ = c.commit(c.t_min)\n"
                 "scene.hits[id.x] = float4(q.hit.t)\n");
    accepted("", "let q = scene.mixed.query(probe(), flags = ray_flags.skip_triangles | ray_flags.cull_back_facing)\n"
                 "for c in q.candidates:\n"
                 "    let _ = c.commit(c.t_min)\n"
                 "scene.hits[id.x] = float4(q.hit.t)\n");
}

TEST("sgl check - a discarded query evaluates its arguments, and a reported query rule is reported once")
{
    // CHK-55: `let _ = world.query(r)` is a query nobody steps through, whatever its arguments
    accepted("fun lane(id: int3) -> int => id.x & 0xff\n",
             "let _ = scene.world.query(probe(), mask = lane(id))\nlet _ = scene.mixed.query(probe())\n"
             "scene.hits[id.x] = float4(1.0)\n");
    // CHK-541: a query a call hands on is reported where it is used, and the scan finds it no second time
    auto const report
        = reports_of_body("fun same(q: trace_query[.triangles]) -> trace_query[.triangles]:\n    return q\n",
                          "let q = scene.world.query(probe())\nfor c in q.candidates:\n    c.commit()\n"
                          "scene.hits[id.x] = float4(same(q).hit.t)\n");
    auto escapes = 0;
    for (auto at = report.find("query-escapes"); at >= 0; at = report.find("query-escapes", at + 1))
        ++escapes;
    CHECK(escapes == 1).dump("report", report);
    // CHK-542: a second loop is misplaced, and no escape besides
    auto const twice
        = reports_of_body("", "let q = scene.world.query(probe())\nfor c in q.candidates:\n    c.commit()\n"
                              "for c in q.candidates:\n    c.commit()\n");
    CHECK(twice.contains("query-traversal-misplaced")).dump("report", twice);
    CHECK(!twice.contains("query-escapes")).dump("report", twice);
}
