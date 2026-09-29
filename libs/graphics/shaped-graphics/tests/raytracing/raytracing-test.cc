#include <clean-core/container/span.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic ray-tracing acceleration-structure builds over the public sg API, run against every available backend.
// These pin the build contract: a BLAS or TLAS builds without device loss, the returned handles are valid and persistent across epochs, and the input validation asserts fire.
// Build sizing is as far as this file goes.
// The trace side is covered end to end by libs/graphics/shaped-shader-compiler-dxc/tests/, which is Windows-only and needs a fetched extern/dxc.
//
// Ray tracing is a device capability, so each test skips when cmd.raytracing.is_supported() is false — a backend without RT, or an adapter that lacks DXR.

namespace
{
// True if this context's backend supports ray-tracing builds (opens + drops a throwaway list to ask).
bool raytracing_supported(sg::context_handle const& ctx)
{
    auto cmd = ctx->create_command_list();
    bool const supported = cmd->raytracing.is_supported();
    ctx->drop_command_list(cc::move(cmd));
    return supported;
}

// A build-input vertex buffer with one triangle (3 float3 positions), filled through ctx.upload.
sg::raw_buffer_handle make_triangle_vertices(sg::context_handle const& ctx)
{
    float const verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    return ctx->persistent.create_buffer_from_data(verts, sg::buffer_usage::accel_structure_build_input).raw();
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - builds a triangle blas and a tlas", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!raytracing_supported(ctx))
        SKIP("ray tracing not supported on this backend/device");

    auto const verts = make_triangle_vertices(ctx);

    sg::blas_triangles tri;
    tri.vertices = verts;
    tri.vertex_count = 3;

    // Build the BLAS then, in the SAME list, a TLAS referencing it.
    // That exercises the intra-list accel_write -> accel_read ordering between the two builds.
    auto cmd = ctx->create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
    REQUIRE(blas != nullptr);

    sg::tlas_instance inst;
    inst.blas = blas;
    inst.instance_id = 7;
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1));
    REQUIRE(tlas != nullptr);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(blas->size_in_bytes() > 0);
    CHECK(blas->geometry_count() == 1);
    CHECK(!blas->is_expired());

    CHECK(tlas->size_in_bytes() > 0);
    CHECK(tlas->instance_count() == 1);
    CHECK(!tlas->is_expired());

    // Persistent: the handles outlive the epoch that built them.
    ctx->advance_epoch();
    co_await ctx->idle_completion();
    CHECK(!blas->is_expired());
    CHECK(!tlas->is_expired());
}

ASYNC_INVOCABLE_TEST("sg - builds a procedural (aabb) blas", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!raytracing_supported(ctx))
        SKIP("ray tracing not supported on this backend/device");

    // One AABB: 6 floats (min.xyz, max.xyz).
    float const aabb[6] = {0, 0, 0, 1, 1, 1};
    auto const buf = ctx->persistent.create_buffer_from_data(aabb, sg::buffer_usage::accel_structure_build_input).raw();

    sg::blas_aabbs g;
    g.aabbs = buf;
    g.aabb_count = 1;

    auto cmd = ctx->create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&g, 1));
    REQUIRE(blas != nullptr);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(blas->size_in_bytes() > 0);
    CHECK(!blas->is_expired());

    // The build input was filled through ctx.upload, whose copy must not outlive the test.
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - acceleration-structure builds validate their inputs", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!raytracing_supported(ctx))
        SKIP("ray tracing not supported on this backend/device");

    auto const verts = make_triangle_vertices(ctx);
    sg::blas_triangles tri;
    tri.vertices = verts;
    tri.vertex_count = 3;

    // fast_trace and fast_build are mutually exclusive.
    {
        auto cmd = ctx->create_command_list();
        CHECK_ASSERTS(cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1),
                                                 sg::accel_build_flag::fast_trace | sg::accel_build_flag::fast_build));
        ctx->drop_command_list(cc::move(cmd));
    }

    // instance_id must fit in 24 bits.
    {
        auto cmd = ctx->create_command_list();
        auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
        sg::tlas_instance inst;
        inst.blas = blas;
        inst.instance_id = 1u << 24; // one past the 24-bit max
        CHECK_ASSERTS(cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1)));
        ctx->drop_command_list(cc::move(cmd));
    }

    // The build input was filled through ctx.upload, whose copy must not outlive the test.
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - builds record what the dispatch_rays hit-record check reads", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!raytracing_supported(ctx))
        SKIP("ray tracing not supported on this backend/device");

    auto const verts = make_triangle_vertices(ctx);
    float const aabb[6] = {0, 0, 0, 1, 1, 1};
    auto const boxes = ctx->persistent.create_buffer_from_data(aabb, sg::buffer_usage::accel_structure_build_input).raw();

    auto const was_on = ctx->portability_checks();
    auto cmd = ctx->create_command_list();

    // The kind and the stride are recorded whatever the checks say, since the BLAS itself carries them.
    auto const tri = sg::blas_triangles{.vertices = verts, .vertex_count = 3};
    auto const triangles
        = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1), sg::accel_build_flag::fast_trace, 2);
    auto const box = sg::blas_aabbs{.aabbs = boxes, .aabb_count = 1};
    auto const procedural = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&box, 1));
    REQUIRE(triangles != nullptr);
    REQUIRE(procedural != nullptr);
    CHECK(triangles->geometry() == sg::blas_geometry::triangles);
    CHECK(triangles->hit_record_stride() == 2);
    CHECK(procedural->geometry() == sg::blas_geometry::aabbs);
    CHECK(procedural->hit_record_stride() == 1);

    sg::tlas_instance const instances[2] = {
        {.blas = triangles, .hit_group_offset = 4},
        {.blas = procedural, .hit_group_offset = 9, .mask = 0x0F},
    };

    // A TLAS keeps its instances for the check only while the checks are on.
    ctx->set_portability_checks(true);
    auto const checked = cmd->raytracing.build_tlas(instances);
    ctx->set_portability_checks(false);
    auto const unchecked = cmd->raytracing.build_tlas(instances);
    ctx->set_portability_checks(was_on);
    ctx->submit_command_list(cc::move(cmd));

    auto const records = sg::impl::instance_records_of(*checked);
    REQUIRE(records.size() == 2);
    CHECK(records[0].blas == triangles);
    CHECK(records[0].hit_group_offset == 4u);
    CHECK(records[1].blas == procedural);
    CHECK(records[1].hit_group_offset == 9u);
    CHECK(records[1].mask == 0x0F);
    CHECK(sg::impl::instance_records_of(*unchecked).empty());

    co_await ctx->idle_completion();
}
