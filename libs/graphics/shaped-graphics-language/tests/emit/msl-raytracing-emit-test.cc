#include "emit-test-support.hh"

#include <clean-core/container/pair.hh>

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
    auto const stages = msl_stages_of(read_text(cc::string(SGL_SG_SHADERS_DIR) + "/raytracing_pipeline.sgl"));
    // the raygen, two misses, three closest hits (one of them the empty one), two any hits, the intersection, and a
    // traversal per ray type of the procedural group
    auto names = cc::string();
    for (auto const& [entry, text] : stages)
        names.appendf("{} ", entry);
    CHECK(names
          == "primary sky open_sky shade cutout shadow_cutout sphere shade_sphere sgl_spheres_surface "
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
    CHECK(text_of(stages, "cutout").contains("[[intersection(triangle, triangle_data, instancing)]]\nbool cutout("));
    CHECK(text_of(stages, "sgl_spheres_surface")
              .contains("[[intersection(bounding_box, triangle_data, instancing)]]\nsgl_box_result "
                        "sgl_spheres_surface("));
    // a traversal's payload is the ray set's, which only the ray data carries into it
    CHECK(text_of(stages, "sgl_spheres_occlusion").contains("ray_data sgl_ray_data& sgl_data [[payload]]"));
}

TEST("sgl emit msl - every stage of sg's callables pipeline writes, and a callable takes no hit record")
{
    auto const stages = msl_stages_of(read_text(cc::string(SGL_SG_SHADERS_DIR) + "/raytracing_callables.sgl"));
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

binding frame:
    output: mut buffer[float]

@raygen fun start(@launch_id id: int3){frame}:
    let sgl_data = instancing((id.x as float) + 1.0)
    frame.output[id.x] = sgl_data.v * 2.0
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
    // the tag itself is still Metal's own, in the tables every stage declares
    CHECK(msl.text.contains("intersection_function_table<triangle_data, instancing>"));
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
