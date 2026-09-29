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
}
