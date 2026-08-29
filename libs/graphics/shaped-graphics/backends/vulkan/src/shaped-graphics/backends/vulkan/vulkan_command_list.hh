#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_inline.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>

/// Vulkan implementation of sg::command_list.
/// Owns its command pool and the single command buffer allocated from it, handed out already recording.
/// Recording is not implemented: every recording call below aborts.
/// The exceptions are the raytracing / query support queries, which honestly answer false, and record_gpu_timestamp, which returns an invalid query.
class sg::backend::vulkan::vulkan_command_list final : public sg::command_list
{
public:
    // Defined in the .cc: the sg::command_list base needs vulkan_context complete to upcast it to sg::context.
    vulkan_command_list(vulkan_context& ctx,
                        sg::epoch created_in,
                        sg::command_list_slot slot,
                        VkCommandPool pool,
                        VkCommandBuffer buffer);

    // Auto-drops, with a warning, a list left neither submitted nor dropped.
    // A no-op once either has run, since both mark the list consumed.
    // Body in vulkan_command_list.cc.
    ~vulkan_command_list() override;

    /// The access-tracking slot this list holds for its lifetime (a backend helper, not sg API).
    [[nodiscard]] sg::command_list_slot slot() const { return _slot; }

    /// Accumulate one declared access for the next op, and enqueue the buffer for the pre-op flush exactly once.
    /// Declaring is separate from flushing so a buffer bound several times to one op produces a single merged barrier.
    void track_buffer_access(vulkan_buffer const& buffer, sg::pipeline_stage_flags stages, sg::access_flags access);

    /// Record one vkCmdPipelineBarrier2 for everything declared since the last flush, then clear the staging.
    /// Called once per op, immediately before recording it.
    void flush_barriers();

    vulkan_context& _ctx;        // creating context — outlives this list
    sg::command_list_slot _slot; // released to the context's slot allocator on submit/drop
    bool _consumed = false;      // set by submit/drop; gates the destructor's auto-drop
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _buffer = VK_NULL_HANDLE; // owned by _pool, freed with it

    // Declared for the current op and awaiting the pre-op flush; empty between ops.
    cc::vector<vulkan_buffer const*> _pending_barrier_buffers;
    cc::vector<VkBufferMemoryBarrier2> _pending_buffer_barriers;

    // Readbacks recorded by this list, still token-less: submit stamps them and hands them to the actor, drop cancels.
    cc::vector<vulkan_download_copy_job> _pending_downloads;

    // Every buffer this list has tracked, so submit can finalize each slot and drop can discard it.
    // Public so the context can walk it at submit; deduplicated by vulkan_buffer::mark_recorded.
    cc::vector<vulkan_buffer const*> _touched_buffers;

protected:
    // Stages the bytes in the context's upload ring and records a copy out of it.
    // Body in vulkan_command_list.cc.
    void upload_bytes_to_buffer(sg::raw_buffer_handle buffer, cc::span<byte const> data, isize offset_in_bytes) override;
    void upload_bytes_to_texture(sg::raw_texture_handle,
                                 cc::span<byte const>,
                                 sg::subresource_index const&,
                                 sg::texture_region const&) override
    {
        CC_UNREACHABLE("vulkan inline texture upload is not implemented yet");
    }
    // Records a copy into the context's readback ring and returns a future the download actor settles.
    // Body in vulkan_command_list.cc.
    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                              isize offset_in_bytes,
                                                              isize size_in_bytes) override;
    [[nodiscard]] sg::bytes_future download_bytes_from_texture(sg::raw_texture_handle,
                                                               sg::subresource_index const&,
                                                               sg::texture_region const&) override
    {
        CC_UNREACHABLE("vulkan inline texture download is not implemented yet");
    }
    // Device-to-device buffer copy.
    // Body in vulkan_command_list.cc.
    void copy_buffer_region(sg::raw_buffer_handle src,
                            sg::raw_buffer_handle dst,
                            isize src_offset_in_bytes,
                            isize dst_offset_in_bytes,
                            isize size_in_bytes) override;

    // Compute recording (reached through cmd.compute) — not implemented yet.
    void compute_bind_pipeline(sg::compute_pipeline const&) override
    {
        CC_UNREACHABLE("vulkan compute bind_pipeline is not implemented yet");
    }
    void compute_bind_group(int, sg::binding_group const&) override
    {
        CC_UNREACHABLE("vulkan compute bind_group is not implemented yet");
    }
    void compute_dispatch(int, int, int) override { CC_UNREACHABLE("vulkan compute dispatch is not implemented yet"); }
    void compute_declare_array_buffer_access(cc::string_view, cc::span<sg::array_buffer_access const>) override
    {
        CC_UNREACHABLE("vulkan compute declare_array_buffer_access is not implemented yet");
    }
    void compute_declare_array_texture_access(cc::string_view, cc::span<sg::array_texture_access const>) override
    {
        CC_UNREACHABLE("vulkan compute declare_array_texture_access is not implemented yet");
    }
    void compute_set_inline_constants(cc::span<byte const>, cc::optional<isize>) override
    {
        CC_UNREACHABLE("vulkan compute set_inline_constants is not implemented yet");
    }

    // Raster rendering scope + draws (reached through cmd.raster / cmd.raster.manual) — not implemented yet.
    void raster_begin_rendering(sg::rendering_info const&) override
    {
        CC_UNREACHABLE("vulkan raster rendering is not implemented yet");
    }
    void raster_end_rendering() override { CC_UNREACHABLE("vulkan raster rendering is not implemented yet"); }
    void raster_bind_pipeline(sg::raster_pipeline const&) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_bind_group(int, sg::binding_group const&) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_bind_vertex_buffers(int, cc::span<sg::vertex_buffer_view const>) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_bind_index_buffer(sg::index_buffer_view const&) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_set_viewport(sg::viewport const&) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_set_scissor(tg::aabb2i const&) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_set_stencil_reference(u32) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_set_blend_constants(tg::vec4f) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_set_inline_constants(cc::span<byte const>, cc::optional<isize>) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_draw(sg::draw_config const&) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_draw_indexed(sg::draw_indexed_config const&) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }

    // Ray tracing (reached through cmd.raytracing) — the recording paths are not implemented yet.
    // is_supported() reports the device's extensions rather than a hardcoded answer, so it already tells the truth
    // about the hardware; it is the build/dispatch stubs below that still have to land.
    // Body in vulkan_command_list.cc, which has vulkan_context complete.
    [[nodiscard]] bool raytracing_is_supported() const override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_triangles(cc::span<sg::blas_triangles const>,
                                                                  sg::accel_build_flags) override
    {
        CC_UNREACHABLE("vulkan raytracing build_blas is not implemented yet");
    }
    [[nodiscard]] sg::blas_handle raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const>, sg::accel_build_flags) override
    {
        CC_UNREACHABLE("vulkan raytracing build_blas is not implemented yet");
    }
    [[nodiscard]] sg::tlas_handle raytracing_build_tlas(cc::span<sg::tlas_instance const>, sg::accel_build_flags) override
    {
        CC_UNREACHABLE("vulkan raytracing build_tlas is not implemented yet");
    }
    void raytracing_bind_pipeline(sg::raytracing_pipeline const&) override
    {
        CC_UNREACHABLE("vulkan raytracing dispatch is not implemented yet");
    }
    void raytracing_bind_group(int, sg::binding_group const&) override
    {
        CC_UNREACHABLE("vulkan raytracing dispatch is not implemented yet");
    }
    void raytracing_dispatch_rays(sg::raytracing_shader_table const&, sg::raygen_index, int, int, int) override
    {
        CC_UNREACHABLE("vulkan raytracing dispatch is not implemented yet");
    }

    // GPU queries (reached through cmd.query) — not implemented yet.
    // Timestamps report unsupported, and record_gpu_timestamp stays callable but always returns an invalid query.
    [[nodiscard]] bool query_timestamps_supported() const override { return false; }
    [[nodiscard]] sg::gpu_timestamp query_record_gpu_timestamp() override { return {}; }
};
