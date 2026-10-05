#include "check-test-support.hh"

using namespace sgl_test;

namespace
{
/// Two ray types, their shaders, and a closest hit that traces the second; each test appends what it is about.
constexpr auto shaders = cc::string_view(
    "require raytracing_pipeline\n"
    "struct radiance:\n    color: float3\n"
    "struct shadow:\n    is_lit: bool\n"
    "rays path_rays:\n    surface: radiance\n    occlusion: shadow\n"
    "binding frame:\n    world: acceleration_structure[.triangles]\n    output: mut buffer[float4]\n"
    "@raygen fun primary(@launch_id id: int3){frame}:\n"
    "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
    "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0), t_min = 0.0, "
    "t_max = 1.0), path_rays.surface, mut p)\n"
    "    frame.output[id.x] = float4(..p.color, 1.0)\n"
    "@miss fun sky(p: mut radiance):\n    p.color = float3(1.0, 1.0, 1.0)\n"
    "@miss fun lit(s: mut shadow):\n    s.is_lit = true\n"
    "@closest_hit fun shade(h: triangle_hit, p: mut radiance){frame}:\n"
    "    let mut s = shadow(false)\n"
    "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 1.0, 0.0), t_min = 0.0, "
    "t_max = 1.0), path_rays.occlusion, mut s)\n"
    "    p.color = h.barycentrics\n"
    "@any_hit fun cutout(c: triangle_candidate, p: mut radiance) -> hit_decision:\n"
    "    return hit_decision.accept\n"
    "@any_hit fun shadow_cutout(c: triangle_candidate, s: mut shadow) -> hit_decision:\n"
    "    return hit_decision.ignore\n"
    "hit_group textured for path_rays:\n"
    "    surface = (closest_hit = shade, any_hit = cutout)\n"
    "    occlusion = (any_hit = shadow_cutout)\n");

/// The settings every pipeline over `path_rays` states, so a test about something else writes them once.
constexpr auto misses = cc::string_view("    rays = path_rays\n    raygen = primary\n    miss.surface = sky\n"
                                        "    miss.occlusion = lit\n");

checked_sources checked(cc::string_view program)
{
    return check_sources(read_prelude(), cc::string(shaders) + program);
}

cc::string reports(cc::string_view program)
{
    return reports_of(checked(program));
}

/// The one ray-tracing pipeline's derived depth, after whatever the check reported.
cc::string depth_of(cc::string_view program)
{
    auto const s = checked(program);
    auto out = reports_of(s);
    for (auto const& p : s.module.pipelines)
        if (p.kind == sgl::check::pipeline_kind::raytracing)
            out.appendf("depth {}\n", p.max_recursion_depth);
    return out;
}
} // namespace

TEST("sgl check - a ray-tracing pipeline derives its depth from what its shaders trace")
{
    // the raygen traces the surface ray, whose closest hit traces the shadow ray: two deep
    CHECK(depth_of(cc::string("@raytracing pipeline path:\n") + misses + "    hit_groups = (textured)\n") == "depth 2\n");
    // without the hit group, nothing traces past the raygen's own ray
    CHECK(depth_of(cc::string("@raytracing pipeline path:\n") + misses) == "depth 1\n");
}

TEST("sgl check - a trace graph with a cycle has no depth")
{
    auto const again = cc::string_view("@closest_hit fun again(h: triangle_hit, p: mut radiance){frame}:\n"
                                       "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, "
                                       "1.0, 0.0), t_min = 0.0, "
                                       "t_max = 1.0), path_rays.surface, mut p)\n"
                                       "hit_group looping for path_rays:\n    surface = (closest_hit = again)\n");
    CHECK(reports(cc::string(again) + "@raytracing pipeline path:\n" + misses + "    hit_groups = (looping)\n")
          == "recursive-trace user:[pipeline path:] a trace of path_rays.surface reaches a shader that traces it "
             "again\n");
}

TEST("sgl check - a cycle no trace of the raygen reaches is still a cycle")
{
    // the raygen traces `surface` alone, and only a host's closest hit could trace `occlusion` into the loop
    auto const looping = cc::string_view("@closest_hit fun again(h: triangle_hit, s: mut shadow){frame}:\n"
                                         "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3("
                                         "0.0, 1.0, 0.0), t_min = 0.0, t_max = 1.0), path_rays.occlusion, mut s)\n"
                                         "hit_group looping for path_rays:\n    occlusion = (closest_hit = again)\n");
    CHECK(reports(cc::string(looping) + "@raytracing pipeline path:\n" + misses
                  + "    hit_groups = (looping, .host)\n    max_recursion_depth = 2\n")
          == "recursive-trace user:[pipeline path:] a trace of path_rays.occlusion reaches a shader that traces it "
             "again\n");
}

TEST("sgl check - a pipeline's shaders trace ray types of its own set alone")
{
    // a trace's contribution, multiplier and miss are positions in one set, and mean nothing in another
    auto const second = cc::string_view("rays second:\n    only: shadow\n"
                                        "@closest_hit fun stray(h: triangle_hit, p: mut radiance){frame}:\n"
                                        "    let mut s = shadow(false)\n"
                                        "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3("
                                        "0.0, 1.0, 0.0), t_min = 0.0, t_max = 1.0), second.only, mut s)\n"
                                        "hit_group astray for path_rays:\n    surface = (closest_hit = stray)\n");
    CHECK(reports(cc::string(second) + "@raytracing pipeline path:\n" + misses + "    hit_groups = (astray)\n")
          == "invalid-pipeline user:[pipeline path:] stray traces second.only, and this pipeline's rays are "
             "path_rays\n");
    // a group no pipeline lists traces what it likes
    CHECK(reports(cc::string(second) + "@raytracing pipeline path:\n" + misses + "    hit_groups = (textured)\n") == "");
}

TEST("sgl check - a trace hands over its ray type's payload as a place")
{
    auto const trace_with = [](cc::string_view payload)
    {
        return reports(cc::string("@raygen fun other(@launch_id id: int3){frame}:\n") + payload
                       + "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0), "
                         "t_min = 0.0, t_max = 1.0), path_rays.surface, mut p)\n");
    };
    CHECK(trace_with("    let mut p = shadow(false)\n")
          == "type-mismatch user:[p] path_rays.surface carries radiance, and this payload is shadow\n");
    CHECK(trace_with("    let p = radiance(float3(0.0, 0.0, 0.0))\n") != "");
    CHECK(reports("@raygen fun other(@launch_id id: int3){frame}:\n"
                  "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
                  "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0), "
                  "t_min = 0.0, t_max = 1.0), path_rays.bounce, mut p)\n")
          == "unknown-member user:[bounce] path_rays has no ray type bounce\n");
}

TEST("sgl check - a hit group's shaders carry the payloads of their ray types")
{
    CHECK(reports("hit_group swapped for path_rays:\n    occlusion = (any_hit = cutout)\n")
          == "invalid-pipeline user:[cutout] cutout takes radiance, and path_rays.occlusion carries shadow\n");
    CHECK(reports("hit_group odd for path_rays:\n    bounce = ()\n")
          == "invalid-pipeline user:[bounce] bounce is no ray type of path_rays\n");
    CHECK(reports("hit_group odd for path_rays:\n    surface = (closest_hit = cutout)\n")
          == "invalid-pipeline user:[cutout] cutout is no @closest_hit entry point\n");
    CHECK(reports("hit_group round for path_rays:\n    geometry = .procedural\n")
          == "invalid-pipeline user:[hit_group round for path_rays:] a procedural hit group has an intersection\n");
}

TEST("sgl check - `.host` hit groups come last and bound the depth")
{
    CHECK(reports(cc::string("@raytracing pipeline path:\n") + misses + "    hit_groups = (.host, textured)\n"
                  + "    max_recursion_depth = 2\n")
          == "invalid-pipeline user:[.host] `.host` stands last among the hit groups\n");
    CHECK(reports(cc::string("@raytracing pipeline path:\n") + misses + "    hit_groups = (textured, .host)\n")
          == "invalid-pipeline user:[pipeline path:] a pipeline with `.host` hit groups declares "
             "`max_recursion_depth`, which "
             "those groups' traces may not exceed\n");
    CHECK(reports(cc::string("@raytracing pipeline path:\n") + misses + "    hit_groups = (textured)\n"
                  + "    max_recursion_depth = 3\n")
          == "invalid-pipeline user:[pipeline path:] max_recursion_depth is derived from what the shaders trace; it is "
             "declared "
             "only beside `.host` hit groups\n");
    CHECK(reports(cc::string("@raytracing pipeline path:\n") + misses + "    hit_groups = (textured, .host)\n"
                  + "    max_recursion_depth = 1\n")
          == "invalid-pipeline user:[pipeline path:] its listed shaders trace 2 deep, past its max_recursion_depth of "
             "1\n");
    CHECK(depth_of(cc::string("@raytracing pipeline path:\n") + misses + "    hit_groups = (textured, .host)\n"
                   + "    max_recursion_depth = 4\n")
          == "depth 4\n");
}

TEST("sgl check - a miss carries its ray type's payload")
{
    CHECK(reports("@raytracing pipeline path:\n    rays = path_rays\n    raygen = primary\n    miss.surface = lit\n")
          == "invalid-pipeline user:[lit] lit takes shadow, and path_rays.surface carries radiance\n");
    CHECK(reports("@raytracing pipeline path:\n    raygen = primary\n")
          == "invalid-pipeline user:[pipeline path:] a @raytracing pipeline names its ray set: `rays = <set>`\n");
}

TEST("sgl check - a procedural group's shaders take what its intersection reports")
{
    auto const spheres
        = cc::string_view("struct sphere_attributes:\n    normal: float3\n"
                          "struct other_attributes:\n    u: float\n"
                          "@intersection fun sphere(b: procedural_box) -> report[sphere_attributes]:\n"
                          "    return report.none()\n"
                          "@closest_hit fun shade_sphere(h: procedural_hit[sphere_attributes], p: mut radiance):\n"
                          "    p.color = h.attributes.normal\n"
                          "@closest_hit fun shade_other(h: procedural_hit[other_attributes], p: mut radiance):\n"
                          "    p.color = float3(h.attributes.u, 0.0, 0.0)\n");
    CHECK(reports(cc::string(spheres) + "hit_group round for path_rays:\n    geometry = .procedural\n"
                  + "    intersection = sphere\n    surface = (closest_hit = shade_sphere)\n")
          == "");
    CHECK(reports(cc::string(spheres) + "hit_group round for path_rays:\n    geometry = .procedural\n"
                  + "    intersection = sphere\n    surface = (closest_hit = shade_other)\n")
          == "invalid-pipeline user:[hit_group round for path_rays:] shade_other takes "
             "procedural_hit[other_attributes], and sphere reports sphere_attributes\n");
    CHECK(reports(cc::string(spheres) + "hit_group mixed_up for path_rays:\n    surface = (closest_hit = shade_sphere)\n")
          == "invalid-pipeline user:[hit_group mixed_up for path_rays:] shade_sphere takes "
             "procedural_hit[sphere_attributes], and this group's geometry is triangles\n");
    // CHK-342: no payload reaches an intersection, and what it reports is a struct
    CHECK(reports("@intersection fun bad(b: procedural_box, p: mut radiance) -> report[float]:\n"
                  "    return report.none()\n")
              .starts_with("invalid-entry-point"));
    // CHK-342: DXR's 32 bytes, a word per scalar, bound what an intersection reports
    CHECK(reports("struct wide:\n    a: float4\n    b: float4\n"
                  "@intersection fun fits(b: procedural_box) -> report[wide]:\n    return report.none()\n")
          == "");
    CHECK(reports("struct too_wide:\n    a: float4\n    b: float4\n    c: float\n"
                  "@intersection fun spills(b: procedural_box) -> report[too_wide]:\n    return report.none()\n")
          == "invalid-entry-point user:[spills] the attributes an @intersection fun reports take 36 bytes, and a "
             "target "
             "holds at most 32\n");
}

TEST("sgl check - a callables table holds callables of one parameter, and a call picks one by index")
{
    auto const callables = cc::string_view("struct operand:\n    x: float\n"
                                           "struct other:\n    y: float\n"
                                           "@callable fun doubled(v: mut operand):\n    v.x = v.x * 2.0\n"
                                           "@callable fun zeroed(v: mut other):\n    v.y = 0.0\n");
    CHECK(reports(cc::string(callables) + "callables ops = (doubled, .host)\n") == "");
    CHECK(reports(cc::string(callables) + "callables ops = (doubled, zeroed)\n")
          == "invalid-pipeline user:[zeroed] zeroed takes other, and this table's callables take operand\n");
    CHECK(reports(cc::string(callables) + "callables ops = (.host, doubled)\n")
          == "invalid-pipeline user:[.host] `.host` stands last among the callables\n");
    // CHK-343: the host's callables follow every listed one, so the table they join is the module's last
    CHECK(reports(cc::string(callables) + "callables ops = (doubled, .host)\ncallables more = (doubled)\n")
          == "invalid-pipeline user:[callables ops = (doubled, .host)] ops takes the host's callables, so it is the "
             "module's last callables table: the host's follow every listed one\n");
    // CHK-344: the call hands over a place of the table's parameter type
    auto const table = cc::string(callables) + "callables ops = (doubled)\n";
    CHECK(reports(table + "@raygen fun go(@launch_id id: int3):\n    let mut v = operand(1.0)\n    ops[id.x](mut v)\n")
          == "");
    CHECK(reports(table + "@raygen fun go(@launch_id id: int3):\n    let mut w = other(1.0)\n    ops[id.x](mut w)\n")
          == "type-mismatch user:[w] ops's callables take operand, and this is other\n");
    CHECK(reports(table + "@raygen fun go(@launch_id id: int3):\n    let v = operand(1.0)\n    ops[id.x](v)\n")
          == "no-matching-overload user:[ops[id.x](v)] a callable takes one place of operand, handed over as `mut "
             "p`\n");
}

TEST("sgl check - a ray-tracing stage that runs once per ray stores into an image, and a candidate's does not")
{
    // the stages a ray runs once each store as compute does: what sv's tracer writes its accumulation and guides from
    auto const image = cc::string_view("binding target:\n    color: out image_2d[.rgba32_float]\n");
    auto const stored = cc::string(image)
                      + "@raygen fun write(@launch_id id: int3){target}:\n"
                        "    target.color.store(int2(id.x, id.y), float4(1.0, 0.0, 0.0, 1.0))\n"
                        "@closest_hit fun write_hit(h: triangle_hit, p: mut radiance){target}:\n"
                        "    target.color.store(int2(0, 0), float4(h.barycentrics.x, 0.0, 0.0, 1.0))\n"
                        "@miss fun write_miss(p: mut radiance){target}:\n"
                        "    target.color.store(int2(0, 0), float4(0.0, 0.0, 1.0, 1.0))\n"
                        "struct operand:\n    x: float\n"
                        "@callable fun write_call(v: mut operand){target}:\n"
                        "    target.color.store(int2(0, 0), float4(v.x, 0.0, 0.0, 1.0))\n";
    CHECK(reports(stored) == "");

    // an any hit and an intersection run any number of times per ray, in any order, so a store there means nothing
    auto const any_hit
        = cc::string(image)
        + "@any_hit fun write_candidate(c: triangle_candidate, p: mut radiance){target} -> hit_decision:\n"
          "    target.color.store(int2(0, 0), float4(1.0, 1.0, 1.0, 1.0))\n"
          "    return hit_decision.accept\n";
    CHECK(reports(any_hit).starts_with("stage-not-allowed"));
    auto const intersection = cc::string(image)
                            + "struct box_attributes:\n    u: float\n"
                              "@intersection fun write_box(b: procedural_box){target} -> report[box_attributes]:\n"
                              "    target.color.store(int2(0, 0), float4(1.0, 1.0, 1.0, 1.0))\n"
                              "    return report.none()\n";
    CHECK(reports(intersection).starts_with("stage-not-allowed"));
}

TEST("sgl check - a pipeline's setting stands once, and hit_groups is a list of groups")
{
    auto const path = cc::string("@raytracing pipeline path:\n") + misses;
    CHECK(reports(path + "    hit_groups = 3\n")
          == "invalid-pipeline user:[hit_groups = 3] hit_groups is a hit group, `.host`, or a round list of them\n");
    CHECK(reports(path + "    hit_groups = .textured\n")
          == "invalid-pipeline user:[.textured] .textured is no hit group\n");
    CHECK(reports(path + "    hit_groups = (a = textured)\n").starts_with("invalid-pipeline"));
    CHECK(reports(path + "    raygen = primary\n") == "invalid-pipeline user:[raygen = primary] raygen is set twice\n");
    CHECK(reports(path + "    rays = path_rays\n") == "invalid-pipeline user:[rays = path_rays] rays is set twice\n");
    CHECK(reports(path + "    hit_groups = (textured)\n    hit_groups = (textured)\n")
          == "invalid-pipeline user:[hit_groups = (textured)] hit_groups is set twice\n");
    CHECK(reports(path + "    hit_groups = (textured, .host)\n    max_recursion_depth = 2\n    max_recursion_depth = 3\n")
          == "invalid-pipeline user:[max_recursion_depth = 3] max_recursion_depth is set twice\n");
}

TEST("sgl check - a hit group holds one record per ray type")
{
    // CHK-330: two lines for one ray type would merge into a record neither line states
    CHECK(reports("hit_group twice for path_rays:\n    surface = (closest_hit = shade)\n    surface = (any_hit = "
                  "cutout)\n")
          == "invalid-pipeline user:[surface = (any_hit = cutout)] surface is set twice\n");
    CHECK(reports("hit_group twice for path_rays:\n    geometry = .triangles\n    geometry = .triangles\n")
          == "invalid-pipeline user:[geometry = .triangles] geometry is set twice\n");
}

TEST("sgl check - a ray set's member is a payload, which is a struct")
{
    // CHK-328: DXC takes no other payload, so the set says so where the type is written
    CHECK(reports("rays loose:\n    x: float\n")
          == "invalid-pipeline user:[float] x is a ray type, whose payload is a struct, and this is float\n");
}

TEST("sgl check - the module's callables share the layout of every ray-tracing pipeline")
{
    // CHK-343: every pipeline holds every table, so a callable's list agrees by position with the pipeline's shaders
    auto const callables = cc::string_view("struct operand:\n    x: float\n"
                                           "binding other:\n    v: float\n"
                                           "@callable fun odd(o: mut operand){other}:\n    o.x = other.v\n"
                                           "callables ops = (odd)\n");
    CHECK(reports(cc::string(callables) + "@raytracing pipeline path:\n" + misses + "    hit_groups = (textured)\n")
          == "invalid-pipeline user:[pipeline path:] group 0 is other to one shader and frame to another: the binding "
             "lists agree by position\n");
    // without a pipeline, a table's layout is nobody's
    CHECK(reports(callables) == "");
}

TEST("sgl check - the attributes a stage names are a struct of the program, of 32 bytes at most")
{
    // CHK-342: in a hit group or not, a procedural hit carries what some intersection reported
    CHECK(reports("struct too_wide:\n    a: float3\n    b: float3\n    c: float3\n"
                  "@closest_hit fun wide(h: procedural_hit[too_wide], p: mut radiance):\n    p.color = "
                  "h.attributes.a\n")
          == "invalid-entry-point user:[wide] the attributes h carries take 36 bytes, and a target holds at most 32\n");
    CHECK(reports("@intersection fun odd(b: procedural_box) -> report[ray]:\n    return report.none()\n")
          == "invalid-entry-point user:[odd] the attributes an @intersection fun reports are a struct of the program, "
             "and ray is none\n");
}

TEST("sgl check - a test that reaches a pipeline's trace is unsupported")
{
    // a test runs no pipeline, and has no tables for the trace to run against
    auto const r = reports("fun shoot(){frame} -> float:\n"
                           "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
                           "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), "
                           "path_rays.surface, mut p)\n"
                           "    return p.color.x\n"
                           "test {frame}:\n    shoot() == 0.0\n");
    CHECK(r.starts_with("unsupported-yet"));
    CHECK(r.contains("a test that reaches trace_ray, which only the ray-tracing stages run"));
}
