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
