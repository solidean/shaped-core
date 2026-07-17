#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

// Inline ray tracing (DXR tier 1.1 RayQuery / TraceRayInline) end to end on WARP, with NO ray-tracing
// pipeline or shader table: an ordinary compute dispatch traces against a bound TLAS. This proves the
// tlas-binding path — binding_type::acceleration_structure, the AS SRV descriptor, and the accel_read
// hazard — is complete on its own.
//
// The scene is one z=0 triangle (0,0,0)-(1,0,0)-(0,1,0). Two threads each shoot a +z ray: thread 0 aims
// at (0.25,0.25) inside the triangle (expects a hit), thread 1 aims at (-1,-1) outside it (expects a
// miss). Everything is driven through the backend-agnostic sg::context API; the WARP device is only how
// the context is created.

namespace
{
// scene is the TLAS (t0); Out holds one hit flag per thread (u0). numthreads matches the 2-thread launch
// so no thread writes past Out.
constexpr char const* inline_rt_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0, space0);
RWStructuredBuffer<uint> Out : register(u0, space0);

[numthreads(2, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float2 xy = (tid.x == 0) ? float2(0.25, 0.25) : float2(-1.0, -1.0);

    RayDesc ray;
    ray.Origin = float3(xy, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 10.0;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed(); // all-opaque geometry commits the closest hit without a candidate loop

    Out[tid.x] = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 1u : 0u;
}
)";
} // namespace

TEST("ssc::dxc + dx12 - inline raytracing traces a bound TLAS in a compute dispatch")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    // WARP implements DXR (incl. tier-1.1 inline RT), but gate on the query so this SKIPs rather than fails
    // on an SDK/device without ray tracing.
    {
        auto probe = ctx.create_command_list();
        bool const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    // Build the scene: one triangle BLAS, one identity-transform TLAS instance. Wait for the GPU so the AS
    // is fully built before the trace reads it.
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

    // Compile the inline-RT compute shader and build a pipeline over its reflected bindings (scene + Out).
    ssc::dxc::shader_description sd;
    sd.stage = sg::shader_stage::compute;
    sd.entry_point = "main";
    sd.model = ssc::dxc::shader_model::sm_6_5; // RayQuery requires SM 6.5+
    sd.source = inline_rt_hlsl;
    auto shader_r = comp.value().compile(sd);
    REQUIRE(shader_r.has_value());
    sg::compiled_shader const shader = cc::move(shader_r.value());

    // Reflection surfaces the TLAS as an acceleration_structure binding.
    bool has_accel = false;
    for (auto const& b : shader.bindings)
        if (b.type == sg::binding_type::acceleration_structure)
            has_accel = true;
    CHECK(has_accel);

    auto group_layout = ctx.uncached.create_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    sg::pipeline_layout_description pld;
    pld.groups = {group_layout};
    auto pipeline_layout = ctx.uncached.create_pipeline_layout(pld);
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

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
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(2);
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

    CHECK(result[0] == 1u); // ray through the triangle hits
    CHECK(result[1] == 0u); // ray outside the triangle misses
}
