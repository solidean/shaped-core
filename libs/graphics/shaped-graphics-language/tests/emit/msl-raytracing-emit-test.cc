#include "emit-test-support.hh"

#include <clean-core/container/pair.hh>
#include <nexus/source_root.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

// EMIT-139: every metal ray-tracing stage is compiled apart and linked, so what one declares the others must agree on.
// These run on every box: the text is the whole of what the emitter decides.

namespace
{
/// Every entry point of `source` written as MSL, by name; each must write.
cc::vector<cc::pair<cc::string, cc::string>> msl_stages_of(cc::string_view source)
{
    auto const checked = check_sources(read_prelude(), source);
    CHECK(reports_of(checked) == "");
    auto result = cc::vector<cc::pair<cc::string, cc::string>>();
    for (auto i = isize(0); i < checked.module.entry_points.size(); ++i)
    {
        auto const e = sgl::emit::emit(checked.module, i, target::msl);
        CHECK(sgl::emit::dump_errors(e) == "");
        result.push_back({checked.module.entry_points[i].name, e.text});
    }
    return result;
}

/// The declaration of `struct name` in `text`, up to its closing brace; empty where there is none.
cc::string struct_text(cc::string_view text, cc::string_view name)
{
    auto const head = cc::format("struct {}\n{{\n", name);
    auto const at = text.find(head);
    if (at < 0)
        return {};
    auto const rest = text.subview(at);
    return cc::string(rest.subview({.offset = 0, .size = rest.find("};\n") + 3}));
}

cc::string_view text_of(cc::span<cc::pair<cc::string, cc::string> const> stages, cc::string_view name)
{
    for (auto const& [entry, text] : stages)
        if (entry == name)
            return text;
    FAIL("no entry point of that name");
    return {};
}
} // namespace

TEST("sgl emit msl - every stage of sg's ray-tracing pipeline writes, and all agree on the ray data")
{
    auto const stages = msl_stages_of(read_text(nx::source_path(SGL_SG_SHADERS_DIR) + "/raytracing_pipeline.sgl"));
    // every stage the file declares, in its order, and then a traversal per ray type of the procedural group and the
    // empty closest hit
    auto names = cc::string();
    for (auto const& [entry, text] : stages)
        names.appendf("{} ", entry);
    CHECK(names
          == "primary sky open_sky shade cutout shadow_cutout sphere shade_sphere reordered_primary "
             "hit_reordered_primary inspected_primary pictured_primary "
             "pictured_sky "
             "counted_primary counted_shade counted_sky asserted_primary asserted_shade asserted_sky "
             "sgl_spheres_surface "
             "sgl_spheres_occlusion sgl_empty_closest_hit ");

    auto const expected = cc::string("struct sgl_ray_data\n{\n"
                                     "    uint4 payload[((sizeof(radiance) > sizeof(shadow) ? sizeof(radiance) : "
                                     "sizeof(shadow)) + 15) / 16];\n");
    auto disagreeing = cc::string();
    for (auto const& [entry, text] : stages)
        if (!struct_text(text, "sgl_ray_data").starts_with(expected))
            disagreeing.appendf("{} ", entry);
    CHECK(disagreeing == "");

    CHECK(text_of(stages, "primary").contains("kernel void primary("));
    CHECK(text_of(stages, "sky")
              .contains("[[visible]] void sky(thread uint4* sgl_payload, thread sgl_hit_record const& "
                        "sgl_hit, thread sgl_context const& sgl_ctx)\n"));
    CHECK(text_of(stages, "cutout")
              .contains("[[intersection(triangle, triangle_data, instancing, world_space_data sgl_motion)]]\n"
                        "sgl_triangle_result cutout("));
    CHECK(text_of(stages, "sgl_spheres_surface")
              .contains("[[intersection(bounding_box, triangle_data, instancing, world_space_data sgl_motion)]]\n"
                        "sgl_box_result sgl_spheres_surface("));
    // a package without SOFTWARE_TRACE is bound no structure that moves, so no stage takes the motion tags
    auto tagged = cc::string();
    for (auto const& [entry, text] : stages)
        if (!text.contains("#define sgl_motion\n") || text.contains("instance_motion"))
            tagged.appendf("{} ", entry);
    CHECK(tagged == "");
    // a traversal's payload is the ray set's, which only the ray data carries into it
    CHECK(text_of(stages, "sgl_spheres_occlusion").contains("ray_data sgl_ray_data& sgl_data [[payload]]"));
}

TEST("sgl emit msl - every stage of sg's callables pipeline writes, and a callable takes no hit record")
{
    auto const stages = msl_stages_of(read_text(nx::source_path(SGL_SG_SHADERS_DIR) + "/raytracing_callables.sgl"));
    CHECK(text_of(stages, "doubled")
              .contains("[[visible]] void doubled(thread uint4* sgl_payload, thread sgl_context const& sgl_ctx)\n"));
    CHECK(text_of(stages, "negated").contains("[[visible]] void negated("));
    CHECK(text_of(stages, "apply_ops").contains("kernel void apply_ops("));
    // the kernel and the miss size the ray data by the pipeline's one ray set
    auto const expected = cc::string("struct sgl_ray_data\n{\n    uint4 payload[(sizeof(probe) + 15) / 16];\n");
    CHECK(struct_text(text_of(stages, "apply_ops"), "sgl_ray_data").starts_with(expected));
    CHECK(struct_text(text_of(stages, "probe_miss"), "sgl_ray_data").starts_with(expected));
}

TEST("sgl emit msl - a file sampler is a constexpr sampler in every ray-tracing stage")
{
    auto const source = cc::string_view(R"(require raytracing_pipeline

sampler clamped:
    filter = .linear
    address = .clamp_edge
    max_lod = 4.0

struct radiance:
    color: float4

rays rs:
    primary: radiance

binding frame:
    world: acceleration_structure[.triangles]
    tex: texture_2d[float4]
    output: mut buffer[float4]

@raygen fun start(@launch_id id: int3){frame}:
    let mut p = radiance(frame.tex.sample(float2(0.5, 0.5), clamped, level = 0.0))
    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), rs.primary, mut p)
    frame.output[id.x] = p.color

@closest_hit fun shade(h: triangle_hit, p: mut radiance){frame}:
    p.color = frame.tex.sample(float2(h.barycentrics.y, h.barycentrics.z), clamped, level = 0.0)

hit_group lit for rs:
    primary = (closest_hit = shade)

@raytracing pipeline path:
    rays = rs
    raygen = start
    hit_groups = (lit)
)");
    auto const stages = msl_stages_of(source);
    auto const declaration = cc::string_view(
        "constexpr sampler clamped(s_address::clamp_to_edge, t_address::clamp_to_edge, r_address::clamp_to_edge, "
        "mag_filter::linear, min_filter::linear, mip_filter::linear, lod_clamp(0.0, 4.0));\n");
    // a visible function has no sampler slot, and the kernel declares it alike so both sample the same
    CHECK(text_of(stages, "shade").contains(declaration));
    CHECK(text_of(stages, "start").contains(declaration));
    CHECK(!text_of(stages, "start").contains("[[sampler("));
}

TEST("sgl emit msl - a ray-tracing stage's file sampler with a mip bias is unsupported")
{
    auto const source = cc::string_view(R"(require raytracing_pipeline

sampler biased:
    mip_lod_bias = 1.0

binding frame:
    tex: texture_2d[float4]
    output: mut buffer[float4]

@raygen fun start(@launch_id id: int3){frame}:
    frame.output[id.x] = frame.tex.sample(float2(0.5, 0.5), biased, level = 0.0)
)");
    auto const e = emit_source(source, 0, target::msl);
    CHECK(sgl::emit::dump_errors(e).contains("unsupported start samples through 'biased', whose mip_lod_bias"));
    CHECK(emit_source(source, 0, target::hlsl_dx12).has_text());
}

TEST("sgl emit - a program name starting with sgl_ is renamed on every target, and MSL reserves its intersection tags")
{
    auto const source = cc::string_view(R"(require raytracing_pipeline

struct instancing:
    v: float

struct instance_motion:
    w: float

binding frame:
    output: mut buffer[float]

@raygen fun start(@launch_id id: int3){frame}:
    let mut sgl_data = instancing((id.x as float) + 1.0)
    if id.x == 0 => sgl_data.v = 0.5
    let mut moved = instance_motion(id.x as float)
    if id.x == 1 => moved.w = 3.0
    frame.output[id.x] = sgl_data.v * 2.0 + moved.w
)");
    auto unrenamed = cc::string();
    for (auto const t : sgl::emit::all_targets())
    {
        if (t == target::wgsl)
            continue; // WebGPU has no ray-tracing pipeline
        auto const e = emit_source(source, 0, t);
        CHECK(sgl::emit::dump_errors(e) == "");
        if (!e.text.contains("sgl_data_") || e.text.contains("sgl_data ="))
            unrenamed.appendf("{} ", sgl::emit::to_string(t));
    }
    CHECK(unrenamed == "");
    auto const msl = emit_source(source, 0, target::msl);
    CHECK(msl.text.contains("struct instancing_\n"));
    CHECK(msl.text.contains("struct instance_motion_\n"));
    CHECK(msl.text.contains("intersection_function_table<triangle_data, instancing, world_space_data sgl_motion>"));
    // the tags themselves are still Metal's own where a package takes SOFTWARE_TRACE, which may bind what moves
    auto const moving = sgl::compile_to_text(
        {.source = source, .entry_point = "start", .target = target::msl, .software_trace = true, .device_features = {}});
    REQUIRE(moving.has_value());
    CHECK(moving.value().text.contains("#define sgl_motion , instance_motion, primitive_motion\n"));
    CHECK(moving.value().text.contains("struct instance_motion_\n"));
}

TEST("sgl emit msl - an any hit two hit groups of differently sized ray sets hold is ray-data-conflict")
{
    auto const source = cc::string_view(R"(require raytracing_pipeline

struct radiance:
    color: float

struct wide:
    values: float4

rays narrow_rays:
    primary: radiance

rays wide_rays:
    primary: radiance
    extra: wide

@any_hit fun keep(c: triangle_candidate, p: mut radiance) -> hit_decision:
    return hit_decision.accept

hit_group narrow_group for narrow_rays:
    primary = (any_hit = keep)

hit_group wide_group for wide_rays:
    primary = (any_hit = keep)
)");
    auto const checked = check_sources(read_prelude(), source);
    CHECK(reports_of(checked) == "");
    auto const e = sgl::emit::emit(checked.module, 0, target::msl);
    CHECK(sgl::emit::dump_errors(e)
          == "ray-data-conflict keep is linked with ray sets 'narrow_rays' and 'wide_rays', whose payloads size "
             "Metal's ray data differently\n");
    // DXR sizes nothing in the text, so the other targets write it
    CHECK(sgl::emit::emit(checked.module, 0, target::hlsl_dx12).has_text());
}

TEST("sgl emit msl - an inline trace outside a ray-tracing stage reaches metal::raytracing, and no member hides its "
     "type")
{
    // compute and pixel stages, each tracing inline: none of them is a ray-tracing stage, and each names the
    // namespace's types, so each must include it
    auto const stages = msl_stages_of(read_text(nx::source_path(SGL_SG_SHADERS_DIR) + "/raytracing.sgl"));
    CHECK(!stages.empty());
    for (auto const& [entry, text] : stages)
    {
        if (!text.contains("instance_acceleration_structure"))
            continue;
        CHECK(text.contains("#include <metal_raytracing>\nusing namespace raytracing;\n")).dump("entry", entry);

        // `ray` is reserved on msl, so the prelude's struct and its members of that name both escape; a member
        // spelled like the struct it holds hides that struct from every member after it (EMIT-138)
        CHECK(!text.contains("ray_ ray_;")).dump("entry", entry);
    }
}

TEST("sgl emit msl - a pipeline trace finds its closest hit in the offsets of the world it traces, whatever names it")
{
    // the world is slot 1 of the entry point's second group, so a writer that guessed would read groups[0][0]
    auto const stages = msl_stages_of(R"(
require raytracing_pipeline, binding_arrays

binding other:
    results: mut buffer[float]

binding scene:
    results: mut buffer[float]
    world: acceleration_structure[.triangles]
    many: acceleration_structure[.triangles][4]

struct probe:
    value: float

rays probe_rays:
    marker: probe

fun probe_ray() => ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0), t_max = 100.0)

fun trace_through(w: acceleration_structure[.triangles], p: mut probe):
    trace(w, probe_ray(), probe_rays.marker, mut p)

@raygen fun direct(@launch_id id: int3){other, scene}:
    let mut p = probe(0.0)
    trace(scene.world, probe_ray(), probe_rays.marker, mut p)
    other.results[id.x] = p.value

@raygen fun through_parameter(@launch_id id: int3){other, scene}:
    let mut p = probe(0.0)
    trace_through(scene.world, mut p)
    other.results[id.x] = p.value

@raygen fun element(@launch_id id: int3){other, scene}:
    let mut p = probe(0.0)
    trace_through(scene.many[nonuniform id.y], mut p)
    other.results[id.x] = p.value

@miss fun missed(p: mut probe):
    p.value = -1.0

@closest_hit fun hit(h: triangle_hit, p: mut probe):
    p.value = h.t

hit_group marked for probe_rays:
    marker = (closest_hit = hit)

@raytracing pipeline direct_path:
    rays = probe_rays
    raygen = direct
    miss.marker = missed
    hit_groups = (marked)

@raytracing pipeline parameter_path:
    rays = probe_rays
    raygen = through_parameter
    miss.marker = missed
    hit_groups = (marked)

@raytracing pipeline element_path:
    rays = probe_rays
    raygen = element
    miss.marker = missed
    hit_groups = (marked)
)");
    CHECK(text_of(stages, "direct").contains("sgl_t.closest_hit[sgl_ctx.hit_offsets->groups[1][1].instances["));
    CHECK(text_of(stages, "through_parameter").contains("sgl_t.closest_hit[sgl_ctx.hit_offsets->groups[1][1].instances["));
    // an element's offsets follow its member's slot by slot
    CHECK(text_of(stages, "element").contains("sgl_ctx.hit_offsets->groups[1][2 + "));
    for (auto const& [entry, text] : stages)
        CHECK(!text.contains("hit_offsets->groups[0]")).dump("entry", entry);
}
