#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

// Full DXR pipeline path end to end on WARP: compile a raygen + miss + closest-hit library each, build a
// raytracing pipeline (state object) and shader table, trace against a one-triangle TLAS, and read back the
// per-ray result. Proves the whole chain — compile -> reflect -> pipeline -> table -> dispatch_rays -> trace.
//
// The scene is one z=0 triangle (0,0,0)-(1,0,0)-(0,1,0). Two rays: ray 0 aims at (0.25,0.25) inside the
// triangle (closest-hit writes 1), ray 1 at (0.9,0.9) outside it (miss writes 0).

namespace
{
// Reads DispatchRaysIndex to pick which ray to shoot, traces it, and stores the payload result. Binds the
// TLAS (t0) and the output buffer (u0) through the global root signature.
constexpr char const* raygen_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> Out : register(u0);

struct Payload { uint value; };

[shader("raygeneration")]
void RayGen()
{
    uint idx = DispatchRaysIndex().x;
    float2 xy = (idx == 0) ? float2(0.25, 0.25) : float2(0.9, 0.9);

    RayDesc ray;
    ray.Origin = float3(xy, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 10.0;

    Payload payload;
    payload.value = 5; // sentinel, overwritten by the hit or miss shader
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    Out[idx] = payload.value;
}
)";

constexpr char const* miss_hlsl = R"(
struct Payload { uint value; };

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.value = 0;
}
)";

constexpr char const* closest_hit_hlsl = R"(
struct Payload { uint value; };
struct Attributes { float2 bary; };

[shader("closesthit")]
void ClosestHit(inout Payload payload, in Attributes attribs)
{
    payload.value = 1;
}
)";

sg::compiled_shader compile_rt(ssc::dxc::compiler& comp, sg::shader_stage stage, cc::string_view entry, cc::string_view source)
{
    ssc::dxc::shader_description desc;
    desc.stage = stage;
    desc.entry_point = cc::string(entry);
    desc.model = ssc::dxc::shader_model::sm_6_3; // DXR pipelines need lib_6_3+
    desc.source = source;
    auto result = comp.compile(desc);
    REQUIRE(result.has_value());
    return cc::move(result.value());
}
} // namespace

TEST("ssc::dxc + dx12 - raytracing pipeline traces a triangle via dispatch_rays")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    {
        auto probe = ctx.create_command_list();
        bool const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    // Build the scene: one triangle BLAS + one identity-transform TLAS instance.
    float const verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto const vbuf = ctx.persistent.create_raw_buffer(
        sizeof(verts), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    {
        auto up = ctx.create_command_list();
        up->upload.data_to_buffer(vbuf, cc::span<float const>(verts, 9));
        ctx.submit_command_list(cc::move(up));
    }

    sg::blas_triangles tri;
    tri.vertices = vbuf;
    tri.vertex_count = 3;

    auto build = ctx.create_command_list();
    auto const blas = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
    REQUIRE(blas != nullptr);
    sg::tlas_instance inst;
    inst.blas = blas;
    auto const tlas = build->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1));
    REQUIRE(tlas != nullptr);
    ctx.submit_command_list(cc::move(build));
    ctx.advance_epoch_and_wait_for_idle();

    // Compile the three ray-tracing shaders (each its own single-entry DXIL library).
    auto raygen = compile_rt(comp.value(), sg::shader_stage::raygen, "RayGen", raygen_hlsl);
    auto miss = compile_rt(comp.value(), sg::shader_stage::miss, "Miss", miss_hlsl);
    auto closest_hit = compile_rt(comp.value(), sg::shader_stage::closest_hit, "ClosestHit", closest_hit_hlsl);

    // Only the raygen shader binds resources (scene + Out); those form the global root signature.
    bool has_accel = false;
    for (auto const& b : raygen.bindings)
        if (b.type == sg::binding_type::acceleration_structure)
            has_accel = true;
    CHECK(has_accel);

    auto group_layout = ctx.uncached.create_binding_group_layout(raygen.bindings);
    REQUIRE(group_layout != nullptr);
    sg::pipeline_layout_description pld;
    pld.groups = {group_layout};
    auto pipeline_layout = ctx.uncached.create_pipeline_layout(pld);
    REQUIRE(pipeline_layout != nullptr);

    // Build the pipeline: register each shader, keeping the returned handles for the shader table.
    sg::raytracing_pipeline_description rpd;
    rpd.layout = pipeline_layout;
    rpd.max_payload_size = sizeof(sg::u32); // one uint payload
    auto const raygen_h = rpd.add_raygen_shader(cc::move(raygen));
    auto const miss_h = rpd.add_miss_shader(cc::move(miss));
    sg::hit_shader hs;
    hs.closest_hit = cc::move(closest_hit);
    auto const hit_h = rpd.add_hit_shader(cc::move(hs));

    auto pipeline = ctx.uncached.create_raytracing_pipeline(rpd);
    REQUIRE(pipeline != nullptr);

    // Build the shader table: one record per section, in the order TraceRay addresses them.
    sg::raytracing_shader_table_description stbd;
    stbd.pipeline = pipeline;
    auto const raygen_idx = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_hit_shader(hit_h);
    auto table = ctx.uncached.create_raytracing_shader_table(stbd);
    REQUIRE(table != nullptr);

    auto out_buf = ctx.persistent.create_raw_buffer(cc::isize(2 * sizeof(sg::u32)),
                                                    sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(out_buf != nullptr);

    sg::named_view const views[] = {
        {.name = "scene", .view = tlas->as_view()},
        {.name = "Out", .view = sg::buffer<sg::u32>::from_raw(out_buf).as_readwrite_buffer()},
    };
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(views, 2));
    REQUIRE(group != nullptr);

    auto disp = ctx.create_command_list();
    disp->raytracing.bind_pipeline(*pipeline);
    disp->raytracing.bind_group(0, *group);
    disp->raytracing.dispatch_rays(*table, raygen_idx, 2); // two rays: one hits, one misses
    ctx.submit_command_list(cc::move(disp));

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<sg::u32>(out_buf, 0, 2);
    ctx.submit_command_list(cc::move(down));
    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());
    cc::vector<sg::u32> result;
    for (auto const v : data.value())
        result.push_back(v);
    REQUIRE(result.size() == 2);

    CHECK(result[0] == 1u); // ray through the triangle runs the closest-hit shader
    CHECK(result[1] == 0u); // ray outside the triangle runs the miss shader
}
