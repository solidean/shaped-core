#include "metal_command_list.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

// Everything below the constructor is a seam the milestone order has not reached; see
// libs/graphics/shaped-graphics/docs/writing-a-backend.md.
//
// A CC_UNREACHABLE stub *satisfies* the CHECK_ASSERTS tests written against these contracts, so those tests pass while
// the seam is unimplemented and start failing the moment it is filled in.
// That is a known trap rather than a surprise: copy the reference backend's assert list when implementing one, rather
// than inferring it from what the tests currently accept.
#define SG_METAL_UNIMPLEMENTED(what) CC_UNREACHABLE(what " is not implemented in the metal backend yet")

namespace sg::backend::metal
{
metal_command_list::metal_command_list(metal_context& ctx,
                                       sg::epoch created_in,
                                       MTL4::CommandAllocator* allocator,
                                       MTL4::CommandBuffer* buffer)
  : sg::command_list(ctx, created_in), _metal_context(ctx), _allocator(allocator), _buffer(buffer)
{
    CC_ASSERT(_allocator != nullptr && _buffer != nullptr, "a metal command list needs both an allocator and a buffer");
    _buffer->beginCommandBuffer(_allocator);
}

metal_command_list::~metal_command_list()
{
    // Ownership normally leaves through release_ownership at submit or drop.
    // A list destroyed still holding it was never handed to the context, so nothing else will free these — release them
    // here rather than leak, and say so, the way the vulkan backend does.
    if (_buffer == nullptr && _allocator == nullptr)
        return;

    CC_LOG_WARNING("command list destroyed without submit or drop — releasing it. Submit or drop every list you open.");

    end_recording();
    if (_buffer != nullptr)
        _buffer->release();
    if (_allocator != nullptr)
        _allocator->release();
}

void metal_command_list::end_recording()
{
    if (!_is_recording)
        return;
    _is_recording = false;
    _buffer->endCommandBuffer();
}

void metal_command_list::release_ownership()
{
    _buffer = nullptr;
    _allocator = nullptr;
}

void metal_command_list::transition_texture_layout(raw_texture_handle,
                                                   texture_layout,
                                                   cc::optional<subresource_range> const&)
{
    SG_METAL_UNIMPLEMENTED("a texture layout transition");
}

void metal_command_list::upload_bytes_to_buffer(raw_buffer_handle, cc::span<byte const>, isize)
{
    SG_METAL_UNIMPLEMENTED("inline buffer upload");
}

void metal_command_list::upload_bytes_to_texture(raw_texture_handle,
                                                 cc::span<byte const>,
                                                 subresource_index const&,
                                                 texture_region const&)
{
    SG_METAL_UNIMPLEMENTED("inline texture upload");
}

sg::bytes_future metal_command_list::download_bytes_from_buffer(raw_buffer_handle, isize, isize)
{
    SG_METAL_UNIMPLEMENTED("inline buffer download");
}

sg::bytes_future metal_command_list::download_bytes_from_texture(raw_texture_handle,
                                                                 subresource_index const&,
                                                                 texture_region const&)
{
    SG_METAL_UNIMPLEMENTED("inline texture download");
}

void metal_command_list::copy_buffer_region(raw_buffer_handle, raw_buffer_handle, isize, isize, isize)
{
    SG_METAL_UNIMPLEMENTED("a device-to-device buffer copy");
}

void metal_command_list::compute_bind_pipeline(compute_pipeline const&)
{
    SG_METAL_UNIMPLEMENTED("binding a compute pipeline");
}

void metal_command_list::compute_bind_group(int, binding_group const&)
{
    SG_METAL_UNIMPLEMENTED("binding a compute binding group");
}

void metal_command_list::compute_dispatch(int, int, int)
{
    SG_METAL_UNIMPLEMENTED("a compute dispatch");
}

void metal_command_list::compute_set_inline_constants(cc::span<byte const>, cc::optional<isize>)
{
    SG_METAL_UNIMPLEMENTED("compute inline constants");
}

void metal_command_list::compute_declare_array_buffer_access(cc::string_view, cc::span<array_buffer_access const>)
{
    SG_METAL_UNIMPLEMENTED("declaring compute array buffer access");
}

void metal_command_list::compute_declare_array_texture_access(cc::string_view, cc::span<array_texture_access const>)
{
    SG_METAL_UNIMPLEMENTED("declaring compute array texture access");
}

void metal_command_list::raster_begin_rendering(rendering_info const&)
{
    SG_METAL_UNIMPLEMENTED("opening a rendering scope");
}

void metal_command_list::raster_end_rendering()
{
    SG_METAL_UNIMPLEMENTED("closing a rendering scope");
}

void metal_command_list::raster_bind_pipeline(raster_pipeline const&)
{
    SG_METAL_UNIMPLEMENTED("binding a raster pipeline");
}

void metal_command_list::raster_bind_group(int, binding_group const&)
{
    SG_METAL_UNIMPLEMENTED("binding a raster binding group");
}

void metal_command_list::raster_bind_vertex_buffers(int, cc::span<vertex_buffer_view const>)
{
    SG_METAL_UNIMPLEMENTED("binding vertex buffers");
}

void metal_command_list::raster_bind_index_buffer(index_buffer_view const&)
{
    SG_METAL_UNIMPLEMENTED("binding an index buffer");
}

void metal_command_list::raster_set_viewport(viewport const&)
{
    SG_METAL_UNIMPLEMENTED("setting the viewport");
}

void metal_command_list::raster_set_scissor(tg::aabb2i const&)
{
    SG_METAL_UNIMPLEMENTED("setting the scissor rect");
}

void metal_command_list::raster_set_stencil_reference(u32)
{
    SG_METAL_UNIMPLEMENTED("setting the stencil reference");
}

void metal_command_list::raster_set_blend_constants(tg::vec4f)
{
    SG_METAL_UNIMPLEMENTED("setting the blend constants");
}

void metal_command_list::raster_set_inline_constants(cc::span<byte const>, cc::optional<isize>)
{
    SG_METAL_UNIMPLEMENTED("raster inline constants");
}

void metal_command_list::raster_draw(draw_config const&)
{
    SG_METAL_UNIMPLEMENTED("a draw");
}

void metal_command_list::raster_draw_indexed(draw_indexed_config const&)
{
    SG_METAL_UNIMPLEMENTED("an indexed draw");
}

bool metal_command_list::raytracing_is_supported() const
{
    // Deliberately false while the build and dispatch seams are stubs, and pinned as deliberate by a tier-2 test.
    // Reporting the device's answer here would turn a clean skip into a crash — see
    // libs/graphics/shaped-graphics/docs/writing-a-backend.md.
    return false;
}

sg::blas_handle metal_command_list::raytracing_build_blas_triangles(cc::span<blas_triangles const>, accel_build_flags)
{
    SG_METAL_UNIMPLEMENTED("building a triangle BLAS");
}

sg::blas_handle metal_command_list::raytracing_build_blas_aabbs(cc::span<blas_aabbs const>, accel_build_flags)
{
    SG_METAL_UNIMPLEMENTED("building a procedural BLAS");
}

sg::tlas_handle metal_command_list::raytracing_build_tlas(cc::span<tlas_instance const>, accel_build_flags)
{
    SG_METAL_UNIMPLEMENTED("building a TLAS");
}

void metal_command_list::raytracing_bind_pipeline(raytracing_pipeline const&)
{
    SG_METAL_UNIMPLEMENTED("binding a ray-tracing pipeline");
}

void metal_command_list::raytracing_bind_group(int, binding_group const&)
{
    SG_METAL_UNIMPLEMENTED("binding a ray-tracing binding group");
}

void metal_command_list::raytracing_dispatch_rays(raytracing_shader_table const&, raygen_index, int, int, int)
{
    SG_METAL_UNIMPLEMENTED("dispatching rays");
}

bool metal_command_list::query_timestamps_supported() const
{
    return false;
}

sg::gpu_timestamp metal_command_list::query_record_gpu_timestamp()
{
    SG_METAL_UNIMPLEMENTED("recording a GPU timestamp");
}
} // namespace sg::backend::metal
