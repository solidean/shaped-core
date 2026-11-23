#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// `source`'s entry point `name`, written for `t`.
cc::string text_of_entry(cc::string_view source, cc::string_view name, target t)
{
    auto const checked = check_sources(read_prelude(), source);
    CHECK(reports_of(checked) == "");
    for (auto i = isize(0); i < checked.module.entry_points.size(); ++i)
        if (checked.module.entry_points[i].name == name)
        {
            auto const e = sgl::emit::emit(checked.module, i, t);
            CHECK(sgl::emit::dump_errors(e) == "");
            return e.text;
        }
    FAIL("no entry point of that name");
    return {};
}
} // namespace

TEST("sgl emit - a payload handed on to a nested trace is written by the stage that hands it on")
{
    // `shade` writes nothing of `color` itself, and the raygen reads what the nested miss wrote through it
    auto const source = cc::string_view(R"(require raytracing_pipeline

struct radiance:
    color: float

rays rs:
    primary: radiance
    bounce: radiance

binding frame:
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

@raygen fun start(@launch_id id: int3){frame}:
    let mut p = radiance(0.0)
    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), rs.primary, mut p)
    frame.output[id.x] = p.color

@miss fun sky2(p: mut radiance):
    p.color = 2.0

@closest_hit fun shade(h: triangle_hit, p: mut radiance){frame}:
    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 1.0, 0.0)), rs.bounce, mut p)

hit_group lit for rs:
    primary = (closest_hit = shade)

@raytracing pipeline path:
    rays = rs
    raygen = start
    miss.bounce = sky2
    hit_groups = (lit)
)");
    // EMIT-137: without `write(closesthit)`, DXR may hand the raygen the value `color` had on entry to `shade`
    CHECK(text_of_entry(source, "shade", target::hlsl_dx12)
              .contains("    float color : read(caller, closesthit, miss) : write(caller, closesthit, miss);\n"));
}

TEST("sgl emit - an inline trace's constant flags are the ray query's template, and forced opaque it decides nothing")
{
    auto const source = cc::string_view(R"(require ray_query

binding frame:
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

@compute(1) fun opaque(@thread_id id: int3){frame}:
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = ray_flags.force_opaque)
    frame.output[id.x] = h.t

@compute(1) fun culled(@thread_id id: int3){frame}:
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = ray_flags.cull_back_facing)
    frame.output[id.x] = h.t

@compute(1) fun chosen(@thread_id id: int3){frame}:
    let mut flags = ray_flags.force_opaque
    if id.x == 0 => flags = ray_flags.none
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = flags)
    frame.output[id.x] = h.t
)");
    // EMIT-159: the template is ORed with the trace's own flags, so naming the constant ones there changes nothing but what a driver may specialize on
    auto const opaque = text_of_entry(source, "opaque", target::hlsl_dx12);
    CHECK(opaque.contains("    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;\n"));
    // every candidate is opaque, so the query is only stepped to its end
    CHECK(opaque.contains("    while (q.Proceed())\n    {\n        continue;\n    }\n"));
    CHECK(!opaque.contains("CandidateType"));

    auto const culled = text_of_entry(source, "culled", target::hlsl_dx12);
    CHECK(culled.contains("    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> "
                          "q;\n"));
    // a comparison, which the `if` around it parenthesizes once
    CHECK(culled.contains("if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)\n"));

    // flags known only at run time leave the template empty, and both loops behind a branch on them
    auto const chosen = text_of_entry(source, "chosen", target::hlsl_dx12);
    CHECK(chosen.contains("    RayQuery<RAY_FLAG_NONE> q;\n"));
    CHECK(chosen.contains("CandidateType"));
}

TEST("sgl emit - an any hit that reads one field of its candidate reads that field alone")
{
    auto const source = cc::string_view(R"(require ray_query

binding frame:
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

fun cutout(c: triangle_candidate) -> hit_decision:
    if c.primitive_index == 1 => return hit_decision.ignore
    return hit_decision.accept

@compute(1) fun main(@thread_id id: int3){frame}:
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), cutout)
    frame.output[id.x] = h.t
)");
    // LEGAL-61 splits the candidate, and L2 drops every read of the query no field it kept needs
    auto const hlsl = text_of_entry(source, "main", target::hlsl_dx12);
    CHECK(hlsl.contains("const int primitive_index = int(q.CandidatePrimitiveIndex());\n"));
    CHECK(!hlsl.contains("CandidateObjectToWorld3x4"));
    CHECK(!hlsl.contains("triangle_candidate c;"));
    auto const msl = text_of_entry(source, "main", target::msl);
    CHECK(!msl.contains("get_candidate_object_to_world_transform"));
}

TEST("sgl emit - a bitwise operand under another bitwise operator is parenthesized")
{
    // DXC refuses `flags & 8 | 512` under `-Wbitwise-op-parentheses`, and WGSL takes no such operand at all
    auto const source = cc::string_view(R"(require raytracing_pipeline

struct radiance:
    color: float

rays rs:
    primary: radiance

binding frame:
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

@raygen fun start(@launch_id id: int3){frame}:
    let mut flags = ray_flags.force_opaque
    if id.x == 0 => flags = ray_flags.cull_opaque
    let mut p = radiance(0.0)
    let kept = (flags & ray_flags.skip_closest_hit) | ray_flags.skip_procedural
    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), rs.primary, mut p, flags = kept)
    frame.output[id.x] = p.color

@miss fun sky(p: mut radiance):
    p.color = 2.0

@raytracing pipeline path:
    rays = rs
    raygen = start
    miss.primary = sky
)");
    CHECK(text_of_entry(source, "start", target::hlsl_dx12).contains("const int kept = (flags & 8) | 512;\n"));
    CHECK(text_of_entry(source, "start", target::msl).contains("const int kept = (flags & 8) | 512;\n"));
}

TEST("sgl emit - a comparison under another comparison is parenthesized")
{
    // WGSL refuses `a < b != c`: its comparisons do not chain, whichever side the inner one stands on
    auto const source = cc::string_view(R"(binding frame:
    values: mut buffer[float]
    flags: mut buffer[int]

@compute(1) fun run(@thread_id id: int3){frame}:
    let below = (frame.values[0] < 0.0) != (frame.flags[0] == 1)
    let above = (frame.flags[0] == 1) != (frame.values[0] > 0.0)
    if below and above => frame.flags[0] = 2
)");
    for (auto const t : sgl::emit::all_targets())
    {
        auto const text = text_of_entry(source, "run", t);
        CHECK(text.contains("< 0.0) != (")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains("== 1) != (")).dump("target", sgl::emit::to_string(t));
    }
}

TEST("sgl emit - an atomic in a raygen, a closest hit, a miss and a callable is each target's own device atomic")
{
    // CHK-404: each of these runs once per launch, hit, miss or call, so an atomic in it counts something defined
    auto const source = cc::string_view(R"(require raytracing_pipeline

struct radiance:
    hit: int

struct operand:
    x: int

rays rs:
    primary: radiance

binding counted:
    world: acceleration_structure[.triangles]
    counts: mut buffer[atomic[uint]]

@callable fun bump(v: mut operand){counted}:
    counted.counts[3].add(1u)
    v.x = v.x + 1

callables ops = (bump)

@raygen fun start(@launch_id id: int3){counted}:
    counted.counts[0].add(1u)
    let mut p = radiance(0)
    trace(counted.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), rs.primary, mut p)
    let mut v = operand(0)
    ops[0](mut v)
    let first = counted.counts[4].compare_exchange_strong(0u, 1u)

@miss fun sky(p: mut radiance){counted}:
    counted.counts[2].add(1u)

@closest_hit fun shade(h: triangle_hit, p: mut radiance){counted}:
    p.hit = 1
    counted.counts[1].add(1u)

hit_group lit for rs:
    primary = (closest_hit = shade)

@raytracing pipeline path:
    rays = rs
    raygen = start
    miss.primary = sky
    hit_groups = (lit)
)");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        CHECK(text_of_entry(source, "start", t).contains("    InterlockedAdd(counted_counts[0], 1u, atomic_before);\n"));
        CHECK(text_of_entry(source, "shade", t).contains("    InterlockedAdd(counted_counts[1], 1u, atomic_before);\n"));
        CHECK(text_of_entry(source, "sky", t).contains("    InterlockedAdd(counted_counts[2], 1u, atomic_before);\n"));
        CHECK(text_of_entry(source, "bump", t).contains("    InterlockedAdd(counted_counts[3], 1u, atomic_before);\n"));
    }
    // a ray-tracing stage has no helpers, so the strong loop needs no test for one (EMIT-162)
    auto const start = text_of_entry(source, "start", target::msl);
    CHECK(start.contains("    (void)(atomic_fetch_add_explicit(&counted_counts[0], 1u, memory_order_relaxed));\n"));
    CHECK(start.contains("    while (!atomic_compare_exchange_weak_explicit(exchange_place, "));
    CHECK(!start.contains("simd_is_helper_thread"));
    cc::string_view const names[] = {"shade", "sky", "bump"};
    for (auto i = 0; i < 3; ++i)
        CHECK(text_of_entry(source, names[i], target::msl)
                  .contains(cc::format("    (void)(atomic_fetch_add_explicit(&counted_counts[{}], 1u, "
                                       "memory_order_relaxed));\n",
                                       i + 1)))
            .context(names[i]);
}

TEST("sgl emit - a return or a yield out of a candidate loop reads the candidate before it aborts the query")
{
    // EMIT-277: the abort comes before the leave, never before the value the leave hands on, which reads the candidate
    auto const source = cc::string_view(R"(require ray_query

binding scene:
    world: acceleration_structure[.triangles]
    hits: mut buffer[float4]

fun first_cut(r: ray){scene} -> int:
    let q = scene.world.query(r)
    for c in q.candidates:
        if c.primitive_index > 3 => return c.primitive_index
    return -1

fun nearest_past(r: ray, n: int){scene} -> float:
    let v = case n:
        0 =>:
            let q = scene.world.query(r)
            for c in q.candidates:
                if c.t > 1.0 => yield c.t
            yield 0.0
        _ => 2.0
    return v

@compute(64) fun main(@thread_id id: int3){scene}:
    let r = ray(origin = pos3(id.x as float, 0.5, -1.0), direction = vec3(0.0, 0.0, 1.0))
    scene.hits[id.x] = float4(first_cut(r) as float, nearest_past(r, id.x), 0.0, 0.0)
)");
    for (auto const t : {target::hlsl_dx12, target::msl})
    {
        auto const text = text_of_entry(source, "main", t);
        auto const view = cc::string_view(text);
        // every abort is followed by no candidate read before the loop's `break`
        for (auto at = view.find(t == target::msl ? ".abort();" : ".Abort();"); at >= 0;
             at = view.find(t == target::msl ? ".abort();" : ".Abort();", at + 1))
        {
            auto const end = view.find("break;", at);
            REQUIRE(end > at);
            auto const read = view.find(t == target::msl ? "get_candidate" : "Candidate", at);
            CHECK((read < 0 || read > end)).dump("text", text);
        }
    }
}

TEST("sgl emit - a commit in the right of an and stays conditional when it ends the search")
{
    // EVAL-80 and CHK-545: `x and c.commit()` commits only where x holds, accept_first_hit_and_end_search or not
    auto const source = cc::string_view(R"(require ray_query

binding scene:
    world: acceleration_structure[.triangles]
    hits: mut buffer[float4]

@compute(64) fun main(@thread_id id: int3){scene}:
    let r = ray(origin = pos3(id.x as float, 0.5, -1.0), direction = vec3(0.0, 0.0, 1.0))
    let q = scene.world.query(r, flags = ray_flags.accept_first_hit_and_end_search)
    let mut asked = 0
    for c in q.candidates:
        if c.t < 10.0 and c.commit() => asked += 100
    scene.hits[id.x] = float4(q.hit.t, asked as float, 0.0, 0.0)
)");
    auto const text = text_of_entry(source, "main", target::hlsl_dx12);
    auto const view = cc::string_view(text);
    auto const test = view.find("q.CandidateTriangleRayT() < 10.0");
    auto const commit = view.find("q.CommitNonOpaqueTriangleHit();");
    REQUIRE(test >= 0);
    REQUIRE(commit >= 0);
    CHECK(test < commit).dump("text", text);
}

TEST("sgl emit - a commit that ends the search marks it only where it commits, in an or, an if or a statement")
{
    // EMIT-277: the mark is as conditional as the commit, so neither comes before the test that guards it
    struct shape
    {
        cc::string_view body;
        cc::string_view test;
    };
    shape const shapes[] = {
        {"        if c.t < 10.0 or c.commit() => n += 1\n", "q.CandidateTriangleRayT() < 10.0"},
        {"        let x = if c.t < 5.0 => c.commit() else false\n        if x => n += 1\n", "q.CandidateTriangleRayT() "
                                                                                            "< 5.0"},
        {"        if c.primitive_index == 2:\n            c.commit()\n", "int(q.CandidatePrimitiveIndex()) == 2"},
    };
    for (auto const& s : shapes)
    {
        auto const source = cc::format(R"(require ray_query

binding scene:
    world: acceleration_structure[.triangles]
    hits: mut buffer[float4]

@compute(64) fun main(@thread_id id: int3){{scene}}:
    let r = ray(origin = pos3(id.x as float, 0.5, -1.0), direction = vec3(0.0, 0.0, 1.0))
    let q = scene.world.query(r, flags = ray_flags.accept_first_hit_and_end_search)
    let mut n = 0
    for c in q.candidates:
{}    scene.hits[id.x] = float4(q.hit.t, n as float, 0.0, 0.0)
)",
                                       s.body);
        auto const text = text_of_entry(source, "main", target::hlsl_dx12);
        auto const view = cc::string_view(text);
        auto const test = view.find(s.test);
        auto const commit = view.find("q.CommitNonOpaqueTriangleHit();");
        auto const mark = view.find("ended = true;");
        REQUIRE(test >= 0);
        REQUIRE(commit >= 0);
        REQUIRE(mark >= 0);
        CHECK(test < commit).dump("text", text);
        CHECK(commit < mark).dump("text", text);
    }
}

TEST("sgl emit - a pixel stage's color, a value block and a loop's value read the candidate before the abort")
{
    // EMIT-277: the minted struct of a pixel stage's return, an `if` returned and a sum over a `loop:`'s value are
    // each evaluated before the abort the return hands on past
    auto const source = cc::string_view(R"(require ray_query

binding scene:
    world: acceleration_structure[.triangles]

struct varyings:
    @position position: hpos4

fun via_block(r: ray){scene} -> float:
    let q = scene.world.query(r)
    for c in q.candidates:
        if c.primitive_index > 2 => return if c.t > 3.0 => c.t else c.t_min
    return -1.0

fun via_loop(r: ray){scene} -> float:
    let q = scene.world.query(r)
    for c in q.candidates:
        let v = loop:
            if c.t > 2.0 => break c.t
            break 0.0
        if v > 0.0 => return v + c.t
    return -1.0

@pixel fun ps(p: varyings){scene} -> float4:
    let r = ray(origin = pos3(p.position.x, 0.5, -1.0), direction = vec3(0.0, 0.0, 1.0))
    let q = scene.world.query(r)
    for c in q.candidates:
        if c.t > 1.0 => return float4(c.t, via_block(r), via_loop(r), 1.0)
    return float4(0.0)
)");
    for (auto const t : {target::hlsl_dx12, target::msl})
    {
        auto const text = text_of_entry(source, "ps", t);
        auto const view = cc::string_view(text);
        auto const abort = t == target::msl ? cc::string_view(".abort();") : cc::string_view(".Abort();");
        auto const read = t == target::msl ? cc::string_view("get_candidate") : cc::string_view("Candidate");
        auto aborts = 0;
        for (auto at = view.find(abort); at >= 0; at = view.find(abort, at + 1))
        {
            ++aborts;
            // what follows the abort is the exit: a `break` of an inlined return, or the stage's own `return`
            auto end = view.find("break;", at);
            auto const returned = view.find("return ", at);
            if (end < 0 || (returned >= 0 && returned < end))
                end = returned;
            REQUIRE(end > at);
            auto const next = view.find(read, at);
            CHECK((next < 0 || next > end)).dump("text", text);
        }
        CHECK(aborts == 3).dump("text", text);
    }
}

TEST("sgl emit - a return that leaves no candidate loop keeps its value where it was")
{
    // EMIT-277: only a leave that aborts a traversal reads its value into a local of its own first
    auto const source = cc::string_view(R"(require ray_query

binding scene:
    world: acceleration_structure[.triangles]
    hits: mut buffer[float4]

fun nearest(r: ray, k: float){scene} -> float:
    let q = scene.world.query(r)
    for c in q.candidates:
        let w = loop:
            break c.t * k
        if w > 100.0 => continue
        c.commit()
    return q.hit.t * k

@compute(64) fun main(@thread_id id: int3){scene}:
    let r = ray(origin = pos3(id.x as float, 0.5, -1.0), direction = vec3(0.0, 0.0, 1.0))
    scene.hits[id.x] = float4(nearest(r, 2.0))
)");
    auto const text = text_of_entry(source, "main", target::hlsl_dx12);
    CHECK(!text.contains("Abort()")).dump("text", text);
    CHECK(!text.contains(" left")).dump("text", text);
}
