#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/backend/command_list_slot.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::command_list. Owns its command pool and the single command buffer
/// allocated from it, handed out already recording. Buffer transfer is not implemented yet.
class vulkan_command_list final : public sg::command_list
{
public:
    vulkan_command_list(vulkan_context& ctx,
                        sg::epoch created_in,
                        sg::command_list_slot slot,
                        VkCommandPool pool,
                        VkCommandBuffer buffer)
      : sg::command_list(created_in), _ctx(ctx), _slot(slot), _pool(pool), _buffer(buffer)
    {
    }

    // Auto-drops (with a warning) a list left neither submitted nor dropped; no-op once either has run
    // (they mark it consumed). Body in vulkan_command_list.cc.
    ~vulkan_command_list() override;

    /// The access-tracking slot this list holds for its lifetime (a backend helper, not sg API).
    [[nodiscard]] sg::command_list_slot slot() const { return _slot; }

    vulkan_context& _ctx;        // creating context — outlives this list
    sg::command_list_slot _slot; // released to the context's slot allocator on submit/drop
    bool _consumed = false;      // set by submit/drop; gates the destructor's auto-drop
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _buffer = VK_NULL_HANDLE; // owned by _pool, freed with it

protected:
    // TODO: inline buffer transfer for the vulkan backend (see the dx12 backend for the reference impl).
    void upload_bytes_to_buffer(sg::raw_buffer_handle, cc::span<cc::byte const>, cc::isize) override
    {
        CC_UNREACHABLE("vulkan inline buffer upload is not implemented yet");
    }
    void upload_bytes_to_texture(sg::raw_texture_handle,
                                 cc::span<cc::byte const>,
                                 sg::subresource_index const&,
                                 sg::texture_region const&) override
    {
        CC_UNREACHABLE("vulkan inline texture upload is not implemented yet");
    }
    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::raw_buffer_handle, cc::isize, cc::isize) override
    {
        CC_UNREACHABLE("vulkan inline buffer download is not implemented yet");
    }
    [[nodiscard]] sg::bytes_future download_bytes_from_texture(sg::raw_texture_handle,
                                                               sg::subresource_index const&,
                                                               sg::texture_region const&) override
    {
        CC_UNREACHABLE("vulkan inline texture download is not implemented yet");
    }
    void copy_buffer_region(sg::raw_buffer_handle, sg::raw_buffer_handle, cc::isize, cc::isize, cc::isize) override
    {
        CC_UNREACHABLE("vulkan inline buffer copy is not implemented yet");
    }

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
    void compute_set_inline_constants(cc::span<cc::byte const>, cc::optional<cc::isize>) override
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
    void raster_set_stencil_reference(sg::u32) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_set_blend_constants(tg::vec4f) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_set_inline_constants(cc::span<cc::byte const>, cc::optional<cc::isize>) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }
    void raster_draw(sg::draw_config const&) override { CC_UNREACHABLE("vulkan raster draw is not implemented yet"); }
    void raster_draw_indexed(sg::draw_indexed_config const&) override
    {
        CC_UNREACHABLE("vulkan raster draw is not implemented yet");
    }

    // Ray tracing (reached through cmd.raytracing) — not implemented yet. is_supported() returns false, so a
    // correct caller never reaches the build stubs; and the vulkan backend stays unregistered in the tier-1
    // API tests until its raytracing milestone lands.
    [[nodiscard]] bool raytracing_is_supported() const override { return false; }
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

    // GPU queries (reached through cmd.query) — not implemented yet. Timestamps report unsupported, so
    // record_gpu_timestamp always returns an invalid query (record is always callable).
    [[nodiscard]] bool query_timestamps_supported() const override { return false; }
    [[nodiscard]] sg::gpu_timestamp query_record_gpu_timestamp() override { return {}; }
};
} // namespace sg::backend::vulkan
