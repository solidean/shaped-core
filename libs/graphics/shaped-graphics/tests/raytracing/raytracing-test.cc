#include <clean-core/container/span.hh>
#include <nexus/test.hh>
#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.raytracing.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic ray-tracing acceleration-structure builds over the public sg API, run against every
// available backend. These pin the build contract: a BLAS/TLAS builds without device loss, the returned
// handles are valid + persistent across epochs, and the input validation asserts fire. Trace/correctness
// needs a raytracing pipeline (deferred), so "builds with plausible sizes" is as far as tier 1 can check.
//
// Ray tracing is a device capability, so each test skips when cmd.raytracing.is_supported() is false (a
// backend without RT, or an adapter that lacks DXR). Vulkan is stubbed + unregistered, so today these run
// on dx12 (WARP, which implements DXR).

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

// A build-input vertex buffer with one triangle (3 float3 positions), uploaded and submitted.
sg::raw_buffer_handle make_triangle_vertices(sg::context_handle const& ctx)
{
    float const verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto const buf = ctx->persistent.create_raw_buffer(
        sizeof(verts), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto cmd = ctx->create_command_list();
    cmd->upload.data_to_buffer(buf, cc::span<float const>(verts, 9));
    ctx->submit_command_list(cc::move(cmd));
    return buf;
}
} // namespace

INVOCABLE_TEST("sg - builds a triangle blas and a tlas", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!raytracing_supported(ctx))
        SKIP("ray tracing not supported on this backend/device");

    auto const verts = make_triangle_vertices(ctx);

    sg::blas_triangles tri;
    tri.vertices = verts;
    tri.vertex_count = 3;

    // Build the BLAS then, in the SAME list, a TLAS referencing it — exercises the intra-list
    // accel_write -> accel_read ordering between the two builds.
    auto cmd = ctx->create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
    REQUIRE(blas != nullptr);

    sg::tlas_instance inst;
    inst.blas = blas;
    inst.instance_id = 7;
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1));
    REQUIRE(tlas != nullptr);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(blas->storage() != nullptr);
    CHECK(blas->size_in_bytes() > 0);
    CHECK(blas->geometry_count() == 1);
    CHECK(!blas->is_expired());

    CHECK(tlas->storage() != nullptr);
    CHECK(tlas->size_in_bytes() > 0);
    CHECK(tlas->instance_count() == 1);
    CHECK(!tlas->is_expired());

    // Persistent: the handles outlive the epoch that built them.
    ctx->advance_epoch_and_wait_for_idle();
    CHECK(!blas->is_expired());
    CHECK(!tlas->is_expired());
}

INVOCABLE_TEST("sg - builds a procedural (aabb) blas", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!raytracing_supported(ctx))
        SKIP("ray tracing not supported on this backend/device");

    // One AABB: 6 floats (min.xyz, max.xyz).
    float const aabb[6] = {0, 0, 0, 1, 1, 1};
    auto const buf = ctx->persistent.create_raw_buffer(
        sizeof(aabb), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    {
        auto up = ctx->create_command_list();
        up->upload.data_to_buffer(buf, cc::span<float const>(aabb, 6));
        ctx->submit_command_list(cc::move(up));
    }

    sg::blas_aabbs g;
    g.aabbs = buf;
    g.aabb_count = 1;

    auto cmd = ctx->create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&g, 1));
    REQUIRE(blas != nullptr);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(blas->storage() != nullptr);
    CHECK(blas->size_in_bytes() > 0);
    CHECK(!blas->is_expired());
}

INVOCABLE_TEST("sg - acceleration-structure builds validate their inputs", (sg::context_handle const& ctx))
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
                                                 sg::accel_build_flags::fast_trace | sg::accel_build_flags::fast_build));
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
}
