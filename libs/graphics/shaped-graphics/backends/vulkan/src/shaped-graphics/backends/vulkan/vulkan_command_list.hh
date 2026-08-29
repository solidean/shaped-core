#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_inline.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>

/// Vulkan implementation of sg::command_list.
/// Owns its command pool and the single command buffer allocated from it, handed out already recording.
/// Recording is not implemented: every recording call below aborts.
/// The exceptions are the raytracing / query support queries, which honestly answer false, and record_gpu_timestamp, which returns an invalid query.
/// One declare_array_buffer_access call, held until the next dispatch resolves it against the bound groups.
struct sg::backend::vulkan::vulkan_array_buffer_declare
{
    cc::string name;
    cc::vector<sg::array_buffer_access> elements;
};

/// One declare_array_texture_access call, the texture analogue of vulkan_array_buffer_declare.
struct sg::backend::vulkan::vulkan_array_texture_declare
{
    cc::string name;
    cc::vector<sg::array_texture_access> elements;
};

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

    /// The texture equivalent, scoped to one subresource range and carrying the layout the op needs it in.
    void track_texture_access(vulkan_texture const& texture,
                              sg::subresource_range range,
                              sg::pipeline_stage_flags stages,
                              sg::access_flags access,
                              sg::texture_layout layout);

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
    cc::vector<vulkan_texture const*> _pending_barrier_textures;
    cc::vector<VkImageMemoryBarrier2> _pending_image_barriers;

    // Readbacks recorded by this list, still token-less: submit stamps them and hands them to the actor, drop cancels.
    cc::vector<vulkan_download_copy_job> _pending_downloads;

    // What compute_bind_* set up and the next dispatch consumes.
    // The layout supplies each slot's schema; the groups supply the resources whose accesses are declared at dispatch.
    vulkan_pipeline_layout const* _bound_pipeline_layout = nullptr;
    cc::vector<vulkan_binding_group const*> _bound_groups;

    // Array bindings are not auto-tracked, so their accesses arrive as explicit declarations and wait here.
    cc::vector<vulkan_array_buffer_declare> _pending_array_buffer_declares;
    cc::vector<vulkan_array_texture_declare> _pending_array_texture_declares;

    // Set inside a rendering scope; a dispatch inside one is rejected because Vulkan forbids it outright.
    bool _in_render_pass = false;

    // The open rendering instance, kept so it can be closed around a barrier and reopened.
    // Vulkan forbids vkCmdPipelineBarrier2 inside a dynamic-rendering instance outright (VUID-...-09553), where
    // D3D12 lets a draw's barriers be flushed in place — so this is what a mid-pass hazard costs here.
    // The load ops are the caller's on the first open and LOAD on every reopen, since the contents are now real.
    cc::fixed_vector<VkRenderingAttachmentInfo, sg::max_color_targets> _rendering_color_attachments;
    VkRenderingAttachmentInfo _rendering_depth_attachment = {};
    VkRenderingAttachmentInfo _rendering_stencil_attachment = {};
    bool _rendering_has_depth = false;
    bool _rendering_has_stencil = false;
    VkRect2D _rendering_area = {};

    // Opens the rendering instance from the stored attachments, forcing every load op to LOAD.
    // Body in vulkan_command_list.raster.cc.
    void reopen_rendering();

    // The graphics bind + input-assembly state, all scoped to the rendering scope that set it up.
    vulkan_pipeline_layout const* _bound_raster_layout = nullptr;
    cc::vector<vulkan_binding_group const*> _bound_raster_groups;
    cc::vector<vulkan_buffer const*> _bound_vertex_buffers;
    vulkan_buffer const* _bound_index_buffer = nullptr;

    // The hazard declares a draw owes: the bound groups' shader accesses plus the input-assembly reads.
    void declare_raster_draw_barriers(bool indexed);

    // Resolves the pending array declares against the bound groups and tracks each named element.
    // Also the accounting pass: a bound array binding with no declaration is an error.
    void declare_array_accesses();

    // Every buffer this list has tracked, so submit can finalize each slot and drop can discard it.
    // Public so the context can walk it at submit; deduplicated by vulkan_buffer::mark_recorded.
    cc::vector<vulkan_buffer const*> _touched_buffers;
    cc::vector<vulkan_texture const*> _touched_textures;

protected:
    // Stages the bytes in the context's upload ring and records a copy out of it.
    // Body in vulkan_command_list.cc.
    void upload_bytes_to_buffer(sg::raw_buffer_handle buffer, cc::span<byte const> data, isize offset_in_bytes) override;
    // Body in vulkan_command_list.cc.
    void upload_bytes_to_texture(sg::raw_texture_handle texture,
                                 cc::span<byte const> pixels,
                                 sg::subresource_index const& subresource,
                                 sg::texture_region const& region) override;
    // Records a copy into the context's readback ring and returns a future the download actor settles.
    // Body in vulkan_command_list.cc.
    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                              isize offset_in_bytes,
                                                              isize size_in_bytes) override;
    // Body in vulkan_command_list.cc.
    [[nodiscard]] sg::bytes_future download_bytes_from_texture(sg::raw_texture_handle texture,
                                                               sg::subresource_index const& subresource,
                                                               sg::texture_region const& region) override;
    // Device-to-device buffer copy.
    // Body in vulkan_command_list.cc.
    void copy_buffer_region(sg::raw_buffer_handle src,
                            sg::raw_buffer_handle dst,
                            isize src_offset_in_bytes,
                            isize dst_offset_in_bytes,
                            isize size_in_bytes) override;

    // Compute recording (reached through cmd.compute). Bodies in vulkan_command_list.compute.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int group_index, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_declare_array_buffer_access(cc::string_view binding_name,
                                             cc::span<sg::array_buffer_access const> elements) override;
    void compute_declare_array_texture_access(cc::string_view binding_name,
                                              cc::span<sg::array_texture_access const> elements) override;
    void compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;

    // Raster rendering scope + draws (reached through cmd.raster / cmd.raster.manual).
    // Bodies in vulkan_command_list.raster.cc.
    void raster_begin_rendering(sg::rendering_info const& info) override;
    void raster_end_rendering() override;
    void raster_bind_pipeline(sg::raster_pipeline const& pipeline) override;
    void raster_bind_group(int group_index, sg::binding_group const& group) override;
    void raster_bind_vertex_buffers(int first_slot, cc::span<sg::vertex_buffer_view const> views) override;
    void raster_bind_index_buffer(sg::index_buffer_view const& view) override;
    void raster_set_viewport(sg::viewport const& vp) override;
    void raster_set_scissor(tg::aabb2i const& rect) override;
    void raster_set_stencil_reference(u32 reference) override;
    void raster_set_blend_constants(tg::vec4f constants) override;
    void raster_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;
    void raster_draw(sg::draw_config const& config) override;
    void raster_draw_indexed(sg::draw_indexed_config const& config) override;

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
