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
