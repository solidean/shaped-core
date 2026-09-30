#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

// Two ray types through hit rows, end to end on dx12: the record math a shading language generates.
//
// The table is ray_count 2, so a trace of ray type r passes r as its ray contribution and 2 as its geometry multiplier.
// Its hit records are E | A B | C E: one single record first, so the rows start at 1 and offset_of is not trivially 0.
// E is the empty hit group, which accepts a hit and runs nothing, and leaves the payload's sentinel in place.
//
// The BLAS has two geometries, and only geometry 1 is in the rays' path.
// The instance takes row 0's offset, so geometry 1 reads record 1 + 1 * 2 + r: C for ray type 0 and E for ray type 1.
// Three rays: ray 0 is type 0 and hits, ray 1 is type 1 and hits, ray 2 is type 0 and misses.

namespace
{
constexpr char const* raygen_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> Out : register(u0);

struct Payload { uint value; };

[shader("raygeneration")]
void RayGen()
{
    uint idx = DispatchRaysIndex().x;
    float2 xy = idx < 2 ? float2(0.25, 0.25) : float2(0.9, 0.9);
    uint ray_type = idx == 1 ? 1 : 0;

    RayDesc ray;
    ray.Origin = float3(xy, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 10.0;

    Payload payload;
    payload.value = 5; // what an accepted hit with no closest-hit shader leaves behind
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, ray_type, 2, 0, ray, payload);
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

/// A closest-hit shader that writes `value`, so each record says which one ran.
[[nodiscard]] cc::string closest_hit_hlsl(u32 value)
{
    return cc::format(R"(
struct Payload {{ uint value; }};
struct Attributes {{ float2 bary; }};

[shader("closesthit")]
void ClosestHit(inout Payload payload, in Attributes attribs)
{{
    payload.value = {};
}}
)",
                      value);
}

constexpr u32 k_sentinel = 5;
constexpr u32 k_miss = 0;
constexpr u32 k_record_c = 30;

sg::compiled_shader compile_rt(ssc::dxc::compiler& comp, sg::shader_stage stage, cc::string_view entry, cc::string_view source)
{
    auto desc = ssc::dxc::shader_description{};
    desc.stage = stage;
    desc.entry_point = cc::string(entry);
    desc.model = ssc::dxc::shader_model::sm_6_3;
    desc.source = source;
    auto result = comp.compile(desc);
    REQUIRE(result.has_value());
    return cc::move(result.value());
}

[[nodiscard]] bool raytracing_supported(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported();
    ctx.drop_command_list(cc::move(probe));
    return supported;
}

struct scene_options
{
    /// Adds an instance of a procedural BLAS outside every ray's path, placed on row 0's triangle records.
    /// No ray reaches it, so the trace is well defined, and the hit-record check still sees the mismatch.
    bool mismatched_instance = false;
    /// Whether the checks are on at dispatch_rays; the scene is always built with them on.
    bool checks_at_dispatch = true;
};

/// Builds the scene, the pipeline and the rows, traces three rays and returns what each wrote.
[[nodiscard]] cc::shared_async<cc::vector<u32>> trace_rows(sg::context& ctx, scene_options options)
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    // Everything the check reads is recorded only while the checks are on.
    auto const was_on = ctx.portability_checks();
    ctx.set_portability_checks(true);

    float const verts[18] = {
        10, 10, 0, 11, 10, 0, 10, 11, 0, // geometry 0, out of every ray's path
        0,  0,  0, 1,  0,  0, 0,  1,  0, // geometry 1, the triangle rays 0 and 1 hit
    };
    auto const vbuf = ctx.persistent.create_buffer_from_data(verts, sg::buffer_usage::accel_structure_build_input).raw();
    sg::blas_triangles const geometries[2] = {
        {.vertices = vbuf, .vertex_count = 3},
        {.vertices = vbuf, .vertex_count = 3, .vertex_offset_in_bytes = 9 * isize(sizeof(float))},
    };

    float const box[6] = {100, 100, 0, 101, 101, 1};
    auto const bbuf = ctx.persistent.create_buffer_from_data(box, sg::buffer_usage::accel_structure_build_input).raw();
    auto const aabbs = sg::blas_aabbs{.aabbs = bbuf, .aabb_count = 1};

    // Compile and build the pipeline first, since the rows decide the instance's offset.
    auto raygen = compile_rt(comp.value(), sg::shader_stage::raygen, "RayGen", raygen_hlsl);
    auto miss = compile_rt(comp.value(), sg::shader_stage::miss, "Miss", miss_hlsl);

    auto group_layout = ctx.cached.acquire_binding_group_layout(raygen.bindings);
    REQUIRE(group_layout != nullptr);
    auto pld = sg::pipeline_layout_description{};
    pld.groups = {group_layout};
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(pld);
    REQUIRE(pipeline_layout != nullptr);

    auto rpd = sg::raytracing_pipeline_description{.layout = pipeline_layout, .max_payload_size = sizeof(u32)};
    auto const raygen_h = rpd.add_raygen_shader(cc::move(raygen));
    auto const miss_h = rpd.add_miss_shader(cc::move(miss));
    auto const closest = [&](u32 value)
    {
        return sg::hit_shader{.closest_hit = compile_rt(comp.value(), sg::shader_stage::closest_hit, "ClosestHit",
                                                        closest_hit_hlsl(value))};
    };
    auto const a = rpd.add_hit_shader(closest(10));
    auto const b = rpd.add_hit_shader(closest(20));
    auto const c = rpd.add_hit_shader(closest(k_record_c));
    auto const e = rpd.add_hit_shader(sg::hit_shader{}); // empty: accepts the hit and runs nothing

    auto pipeline = co_await ctx.cached.acquire_raytracing_pipeline(rpd);
    REQUIRE(pipeline != nullptr);

    auto stbd = sg::raytracing_shader_table_description{.pipeline = pipeline, .ray_count = 2};
    auto const raygen_idx = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_hit_shader(e);
    sg::hit_shader_handle const first_row[2] = {a, b};
    sg::hit_shader_handle const second_row[2] = {c, e};
    auto const row0 = stbd.add_hit_row(first_row);
    (void)stbd.add_hit_row(second_row);
    auto table = ctx.uncached.create_raytracing_shader_table(stbd);
    REQUIRE(table != nullptr);
    CHECK(table->offset_of(row0) == 1u);

    auto build = ctx.create_command_list();
    auto const blas = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(geometries),
                                                   sg::accel_build_flag::fast_trace, 2);
    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back({.blas = blas, .hit_group_offset = table->offset_of(row0)});
    if (options.mismatched_instance)
    {
        auto const procedural = build->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&aabbs, 1),
                                                             sg::accel_build_flag::fast_trace, 2);
        instances.push_back({.blas = procedural, .hit_group_offset = table->offset_of(row0)});
    }
    auto const tlas = build->raytracing.build_tlas(instances);
    REQUIRE(tlas != nullptr);
    ctx.submit_command_list(cc::move(build));

    auto out_buf = ctx.persistent.create_raw_buffer(isize(3 * sizeof(u32)),
                                                    sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    sg::named_view const views[] = {
        {.name = "scene", .view = tlas->as_view()},
        {.name = "Out", .view = sg::buffer<u32>::from_raw(out_buf).as_readwrite_buffer()},
    };
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(views));
    REQUIRE(group != nullptr);

    ctx.set_portability_checks(options.checks_at_dispatch);
    auto disp = ctx.create_command_list();
    disp->raytracing.bind_pipeline(*pipeline);
    disp->raytracing.bind_group(0, *group);
    disp->raytracing.dispatch_rays(*table, raygen_idx, 3);
    ctx.submit_command_list(cc::move(disp));
    ctx.set_portability_checks(was_on);

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<u32>(out_buf, 0, 3);
    ctx.submit_command_list(cc::move(down));
    auto const data = co_await future.data();
    auto result = cc::vector<u32>();
    for (auto const v : data)
        result.push_back(v);
    co_return result;
}

void check_record_math(cc::span<u32 const> result)
{
    REQUIRE(result.size() == 3);
    CHECK(result[0] == k_record_c).context(cc::format("ray type 0 read {}, expected record C", result[0]));
    CHECK(result[1] == k_sentinel)
        .context(cc::format("ray type 1 read {}, expected the empty group to leave the sentinel", result[1]));
    CHECK(result[2] == k_miss).context(cc::format("the missing ray read {}", result[2]));
}
} // namespace

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - two ray types reach their records through hit rows",
                     (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    if (!raytracing_supported(*handle))
        SKIP("device reports no ray tracing support");

    // The checks are on and every reached record fits its BLAS, so nothing may be logged.
    auto const result = co_await trace_rows(*handle, {});
    check_record_math(result);
}

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - dispatch_rays logs an instance whose BLAS kind its records do not match",
                     (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    if (!raytracing_supported(*handle))
        SKIP("device reports no ray tracing support");

    // Logged and not asserted, and the trace still runs: the hit groups come from shaders hot reload can change.
    nx::expect_error("hit records of the wrong kind", nx::exactly(1, "sg"));
    auto const result = co_await trace_rows(*handle, {.mismatched_instance = true});
    check_record_math(result);
}

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - dispatch_rays checks no hit records while portability checks are off",
                     (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    if (!raytracing_supported(*handle))
        SKIP("device reports no ray tracing support");

    // The same mismatch as above, with everything recorded; only the dispatch runs with the checks off.
    auto const result = co_await trace_rows(*handle, {.mismatched_instance = true, .checks_at_dispatch = false});
    check_record_math(result);
}
