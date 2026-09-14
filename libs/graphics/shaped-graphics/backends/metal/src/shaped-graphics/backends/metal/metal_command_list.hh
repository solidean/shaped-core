#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_barrier.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>

/// Metal implementation of sg::command_list.
///
/// One MTL4 command buffer recording into one leased MTL4 command allocator, both handed back to the epoch system at
/// submit — the allocator cannot be reset while a buffer built from it is still executing, which is what makes the
/// epoch rather than the submit the right moment.
///
/// Recording is not implemented yet: every seam below the transfer line asserts.
/// See libs/graphics/shaped-graphics/docs/writing-a-backend.md for the milestone order it is being filled in along.
class sg::backend::metal::metal_command_list final : public sg::command_list
{
public:
    metal_command_list(metal_context& ctx,
                       sg::epoch created_in,
                       MTL4::CommandAllocator* allocator,
                       MTL4::CommandBuffer* buffer);
    ~metal_command_list() override;

    [[nodiscard]] MTL4::CommandBuffer* buffer() const { return _buffer; }
    [[nodiscard]] MTL4::CommandAllocator* allocator() const { return _allocator; }

    /// Closes recording, so the buffer may be committed.
    /// Idempotent.
    void end_recording();

    /// This list's slot in every resource's concurrent access tracking.
    [[nodiscard]] sg::command_list_slot slot() const { return _slot; }

    /// The buffers this list declared against, each needing a finalize at submit or a discard at drop.
    [[nodiscard]] cc::span<sg::raw_buffer_handle const> touched_buffers() const { return _touched_buffers; }

    /// The copy-outs this list's downloads are waiting on, handed to the submit that will run them.
    /// Moved out, so the list keeps none afterwards.
    [[nodiscard]] cc::vector<cc::unique_function<void()>> take_pending_downloads()
    {
        return cc::move(_pending_downloads);
    }

    /// Hands the allocator and the buffer over; the list owns neither afterwards.
    /// Called by the context once it has taken responsibility for them, whether the list is submitted or dropped.
    void release_ownership();

private:
    void transition_texture_layout(raw_texture_handle texture,
                                   texture_layout layout,
                                   cc::optional<subresource_range> const& range) override;

    void upload_bytes_to_buffer(raw_buffer_handle buffer, cc::span<byte const> data, isize offset_in_bytes) override;
    void upload_bytes_to_texture(raw_texture_handle texture,
                                 cc::span<byte const> pixels,
                                 subresource_index const& subresource,
                                 texture_region const& region) override;
    [[nodiscard]] bytes_future download_bytes_from_buffer(raw_buffer_handle buffer,
                                                          isize offset_in_bytes,
                                                          isize size_in_bytes) override;
    [[nodiscard]] bytes_future download_bytes_from_texture(raw_texture_handle texture,
                                                           subresource_index const& subresource,
                                                           texture_region const& region) override;
    void copy_buffer_region(raw_buffer_handle src,
                            raw_buffer_handle dst,
                            isize src_offset_in_bytes,
                            isize dst_offset_in_bytes,
                            isize size_in_bytes) override;

    void compute_bind_pipeline(compute_pipeline const& pipeline) override;
    void compute_bind_group(int group_index, binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;
    void compute_declare_array_buffer_access(cc::string_view binding_name,
                                             cc::span<array_buffer_access const> elements) override;
    void compute_declare_array_texture_access(cc::string_view binding_name,
                                              cc::span<array_texture_access const> elements) override;

    void raster_begin_rendering(rendering_info const& info) override;
    void raster_end_rendering() override;
    void raster_bind_pipeline(raster_pipeline const& pipeline) override;
    void raster_bind_group(int group_index, binding_group const& group) override;
    void raster_bind_vertex_buffers(int first_slot, cc::span<vertex_buffer_view const> views) override;
    void raster_bind_index_buffer(index_buffer_view const& view) override;
    void raster_set_viewport(viewport const& vp) override;
    void raster_set_scissor(tg::aabb2i const& rect) override;
    void raster_set_stencil_reference(u32 reference) override;
    void raster_set_blend_constants(tg::vec4f constants) override;
    void raster_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;
    void raster_draw(draw_config const& config) override;
    void raster_draw_indexed(draw_indexed_config const& config) override;

    [[nodiscard]] bool raytracing_is_supported() const override;
    [[nodiscard]] blas_handle raytracing_build_blas_triangles(cc::span<blas_triangles const> geometries,
                                                              accel_build_flags flags) override;
    [[nodiscard]] blas_handle raytracing_build_blas_aabbs(cc::span<blas_aabbs const> geometries,
                                                          accel_build_flags flags) override;
    [[nodiscard]] tlas_handle raytracing_build_tlas(cc::span<tlas_instance const> instances,
                                                    accel_build_flags flags) override;
    void raytracing_bind_pipeline(raytracing_pipeline const& pipeline) override;
    void raytracing_bind_group(int group_index, binding_group const& group) override;
    void raytracing_dispatch_rays(raytracing_shader_table const& table,
                                  raygen_index raygen,
                                  int width,
                                  int height,
                                  int depth) override;

    [[nodiscard]] bool query_timestamps_supported() const override;
    [[nodiscard]] gpu_timestamp query_record_gpu_timestamp() override;

    /// The encoder every copy and dispatch records into, opened on first use.
    ///
    /// **Metal 4 has no blit encoder.** Its compute encoder carries `copyFromBuffer`, `copyFromTexture` and
    /// `fillBuffer` alongside dispatch, where dx12 and vulkan each have a distinct copy path — so one encoder serves
    /// both, and a barrier on it may name `MTLStageBlit` and `MTLStageDispatch` alike.
    [[nodiscard]] MTL4::ComputeCommandEncoder* compute_encoder();

    /// Closes the open encoder, if any.
    /// A render pass needs the compute one closed first.
    void end_encoder();

    /// Declare `access` on `buffer` for the op about to be recorded, and remember it for the finalize at submit.
    void declare_buffer(raw_buffer_handle const& buffer, pipeline_stage_flags stages, access_flags access);

    /// Emit the barriers every buffer declared since the last flush needs, then clear the declares.
    /// Called immediately before the op those declares were for.
    void flush_barriers();

    metal_context& _metal_context;
    MTL4::CommandAllocator* _allocator = nullptr;
    MTL4::CommandBuffer* _buffer = nullptr;
    MTL4::ComputeCommandEncoder* _encoder = nullptr;
    sg::command_list_slot _slot = sg::command_list_slot::invalid;
    bool _is_recording = true;

    /// Whether this list recorded anything the next list may have to wait for.
    /// Decides whether the producer half of the queue barrier pair is worth emitting at all.
    bool _produced_queue_work = false;

    /// Every buffer this list declared against, held so the resource outlives the recording that names it.
    cc::vector<sg::raw_buffer_handle> _touched_buffers;

    /// The buffers with a declare awaiting the next flush.
    /// A subset of `_touched_buffers`, cleared per op.
    cc::vector<sg::raw_buffer_handle> _pending_buffers;

    /// One per download recorded: copies the bytes out of the staging ring and settles the future.
    ///
    /// **Run when the commit completes, not when the epoch retires.**
    /// `ctx.block_until_idle()` drains the GPU without advancing, and an open epoch's payload never runs — so a
    /// deferral onto the epoch would leave every download in an unadvanced frame unsettled forever.
    cc::vector<cc::unique_function<void()>> _pending_downloads;
};
