#include <clean-core/common/log.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh> // cc::async_blocking_get
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

// Q13: how does a ray payload pack, and does anything enforce the size we declare for it?
//
// The binding preprocessor computes a payload's size from its struct, because nothing in sg::compiled_shader
// reports one — see libs/graphics/shaped-shader-library/docs/binding-preprocessor.md.
// Which size it computes depends on a fact we do not get to choose: a payload is registers rather than a
// buffer, so it *should* pack at natural alignment rather than in a constant buffer's 16-byte rows.
// "Should" is what this file removes.
//
// The payload below is chosen so the two rules disagree by eight bytes:
//
//   struct { float2 a; float3 b; uint tag; }
//     natural:         a at 0, b at 8, tag at 20   -> 24 bytes
//     constant buffer: a at 0, b at 16, tag at 28  -> 32 bytes  (b may not straddle a 16-byte row)
//
// THE ANSWER: natural alignment.
// CreateStateObject accepts 24 and traces every field through intact, and refuses both 20 and 4 — so 24 is
// not merely sufficient, it is the minimum, which is the only reading that says how the driver packed it.
// A larger declared size is always legal, so nothing but the minimum could have answered this.

namespace
{
// The scene and the trace are the smallest that reach a closest-hit shader: one z=0 triangle, one ray into it.
constexpr char const* payload_raygen_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> Out : register(u0);

struct Payload { float2 a; float3 b; uint tag; };

[shader("raygeneration")]
void RayGen()
{
    RayDesc ray;
    ray.Origin = float3(0.25, 0.25, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 10.0;

    Payload payload;
    payload.a = float2(0, 0);
    payload.b = float3(0, 0, 0);
    payload.tag = 0;
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    // Every field, so a payload the driver truncated shows up as a wrong number rather than as nothing.
    Out[0] = uint(payload.a.x);
    Out[1] = uint(payload.a.y);
    Out[2] = uint(payload.b.x);
    Out[3] = uint(payload.b.y);
    Out[4] = uint(payload.b.z);
    Out[5] = payload.tag;
}
)";

constexpr char const* payload_miss_hlsl = R"(
struct Payload { float2 a; float3 b; uint tag; };

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.tag = 99;
}
)";

// Writes a distinct value into every field, the last one deliberately past where a 24-byte payload would end
// if the driver had packed it in 16-byte rows.
constexpr char const* payload_hit_hlsl = R"(
struct Payload { float2 a; float3 b; uint tag; };
struct Attributes { float2 bary; };

[shader("closesthit")]
void ClosestHit(inout Payload payload, in Attributes attribs)
{
    payload.a = float2(1, 2);
    payload.b = float3(3, 4, 5);
    payload.tag = 6;
}
)";

[[nodiscard]] sg::compiled_shader compile_rt(ssc::dxc::compiler& comp,
                                             sg::shader_stage stage,
                                             cc::string_view entry,
                                             cc::string_view source)
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

/// The whole experiment for one declared payload size.
/// Returns the six values the raygen shader stored, or nothing when the pipeline would not build — which is
/// itself an answer, and the one the undersized case is looking for.
[[nodiscard]] cc::optional<cc::vector<u32>> trace_with_payload_size(sg::context& ctx, isize max_payload_size)
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

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

    auto raygen = compile_rt(comp.value(), sg::shader_stage::raygen, "RayGen", payload_raygen_hlsl);
    auto miss = compile_rt(comp.value(), sg::shader_stage::miss, "Miss", payload_miss_hlsl);
    auto hit = compile_rt(comp.value(), sg::shader_stage::closest_hit, "ClosestHit", payload_hit_hlsl);

    auto group_layout = ctx.cached.acquire_binding_group_layout(raygen.bindings);
    REQUIRE(group_layout != nullptr);
    sg::pipeline_layout_description pld;
    pld.groups = {group_layout};
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(pld);
    REQUIRE(pipeline_layout != nullptr);

    sg::raytracing_pipeline_description rpd;
    rpd.layout = pipeline_layout;
    rpd.max_payload_size = max_payload_size;
    auto const raygen_h = rpd.add_raygen_shader(cc::move(raygen));
    auto const miss_h = rpd.add_miss_shader(cc::move(miss));
    sg::hit_shader hs;
    hs.closest_hit = cc::move(hit);
    auto const hit_h = rpd.add_hit_shader(cc::move(hs));

    auto pipeline_result = cc::try_async_blocking_get(ctx.cached.acquire_raytracing_pipeline(rpd));
    if (pipeline_result.has_error())
    {
        CC_LOG_INFO("[spike] Q13 max_payload_size={} refused: {}", max_payload_size,
                    pipeline_result.error().underlying().to_string());
        return cc::nullopt;
    }

    auto const pipeline = pipeline_result.value();
    REQUIRE(pipeline != nullptr);

    sg::raytracing_shader_table_description stbd;
    stbd.pipeline = pipeline;
    auto const raygen_idx = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_hit_shader(hit_h);
    auto table = ctx.uncached.create_raytracing_shader_table(stbd);
    REQUIRE(table != nullptr);

    auto out_buf = ctx.persistent.create_raw_buffer(isize(6 * sizeof(u32)),
                                                    sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(out_buf != nullptr);

    sg::named_view const views[] = {
        {.name = "scene", .view = tlas->as_view()},
        {.name = "Out", .view = sg::buffer<u32>::from_raw(out_buf).as_readwrite_buffer()},
    };
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(views, 2));
    REQUIRE(group != nullptr);

    auto disp = ctx.create_command_list();
    disp->raytracing.bind_pipeline(*pipeline);
    disp->raytracing.bind_group(0, *group);
    disp->raytracing.dispatch_rays(*table, raygen_idx, 1);
    ctx.submit_command_list(cc::move(disp));

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<u32>(out_buf, 0, 6);
    ctx.submit_command_list(cc::move(down));
    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());

    cc::vector<u32> result;
    for (auto const v : data.value())
        result.push_back(v);
    return result;
}
} // namespace

INVOCABLE_TEST("ssc::dxc + dx12 - Q13 a ray payload packs at natural alignment, not in 16-byte rows",
               (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    {
        auto probe = ctx.create_command_list();
        bool const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    // 24 is the natural-alignment size; the constant-buffer rules would make the same struct 32.
    // Every field arriving intact is what says the driver reserved a scalar-packed payload.
    auto const traced = trace_with_payload_size(ctx, 24);
    REQUIRE(traced.has_value());

    auto const& out = traced.value();
    REQUIRE(out.size() == 6);
    CHECK(out[0] == 1u); // a.x
    CHECK(out[1] == 2u); // a.y
    CHECK(out[2] == 3u); // b.x
    CHECK(out[3] == 4u); // b.y
    CHECK(out[4] == 5u); // b.z
    CHECK(out[5] == 6u); // tag — the field a 16-byte-row payload would have pushed past 24 bytes
}

INVOCABLE_TEST("ssc::dxc + dx12 - Q13b 24 is the SMALLEST size the payload is accepted at",
               (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    {
        auto probe = ctx.create_command_list();
        bool const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    // This is what turns Q13 from "24 is enough" into "24 is what the payload takes", and it is the whole
    // discriminator: a larger declared size is always legal, so only the minimum says how the driver packed it.
    //
    // 20 is one field short of natural alignment, and far short of the 32 the constant-buffer rules would ask for.
    // Refusing it while accepting 24 leaves natural alignment as the only reading.
    CHECK(!trace_with_payload_size(ctx, 20).has_value());

    // And far too little, so the refusal above is not some quirk of being four bytes out.
    CHECK(!trace_with_payload_size(ctx, 4).has_value());
}
