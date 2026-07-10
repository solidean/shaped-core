#include "dx12-test-common.hh"

#include <clean-core/container/span.hh>
#include <nexus/test.hh>
#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// dx12-specific ray-tracing smoke: build a triangle BLAS and a single-instance TLAS on WARP (which
// implements DXR), then reach into the concrete dx12_blas/dx12_tlas to check the backend internals a
// tier-1 test can't see — the prebuild sizes and the storage GPU virtual address. Drives everything through
// the abstract sg API; casts only to inspect (the tier-2 rule from the sg testing guidelines).

namespace
{
namespace dx12 = sg::backend::dx12;

sg::raw_buffer_handle upload_triangle_vertices(sg::context_handle const& ctx)
{
    float const verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto const buf = ctx->persistent.create_raw_buffer(
        sizeof(verts), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto up = ctx->create_command_list();
    up->upload.data_to_buffer(buf, cc::span<float const>(verts, 9));
    ctx->submit_command_list(cc::move(up));
    return buf;
}
} // namespace

TEST("sg dx12 - raytracing builds a blas and a tlas on WARP")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);

    // WARP implements DXR, but gate on the query so the test SKIPs (not fails) if this SDK's WARP doesn't.
    {
        auto probe = handle->create_command_list();
        bool const supported = probe->raytracing.is_supported();
        handle->drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("WARP reports no ray tracing support on this SDK");
    }

    auto const verts = upload_triangle_vertices(handle);

    sg::blas_triangles tri;
    tri.vertices = verts;
    tri.vertex_count = 3;

    auto cmd = handle->create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
    REQUIRE(blas != nullptr);

    sg::tlas_instance inst;
    inst.blas = blas;
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1));
    REQUIRE(tlas != nullptr);
    handle->submit_command_list(cc::move(cmd));
    handle->advance_epoch_and_wait_for_idle(); // let the builds finish on the GPU

    // Inspect the backend internals: concrete type, non-zero prebuild sizes, live storage GPU VA.
    auto const dx_blas = std::dynamic_pointer_cast<dx12::dx12_blas const>(blas);
    REQUIRE(dx_blas != nullptr);
    CHECK(dx_blas->size_in_bytes() > 0);
    CHECK(dx_blas->build_scratch_size_in_bytes() > 0);
    CHECK(dx_blas->_dx12_storage->gpu_virtual_address() != 0);

    auto const dx_tlas = std::dynamic_pointer_cast<dx12::dx12_tlas const>(tlas);
    REQUIRE(dx_tlas != nullptr);
    CHECK(dx_tlas->size_in_bytes() > 0);
    CHECK(dx_tlas->build_scratch_size_in_bytes() > 0);
    CHECK(dx_tlas->instance_count() == 1);
    CHECK(dx_tlas->_dx12_storage->gpu_virtual_address() != 0);
}
