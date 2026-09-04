#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_inline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_query.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>

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

/// Vulkan implementation of sg::command_list.
/// Owns its command pool and the single command buffer allocated from it, handed out already recording.
/// Every scope records for real — transfer, copy, compute, raster, ray tracing and queries — and the capability
/// queries report the device rather than a fixed answer: cmd.raytracing.is_supported() asks the context's extension
/// set, and cmd.query.is_supported() the queue family's timestamp bits.
class sg::backend::vulkan::vulkan_command_list final : public sg::command_list
{
public:
    // Defined in the .cc: the sg::command_list base needs vulkan_context complete to upcast it to sg::context.
    vulkan_command_list(vulkan_context& ctx,
                        sg::epoch created_in,
                        sg::command_list_slot slot,
                        VkCommandPool pool,
                        VkCommandBuffer buffer,
                        VkCommandBuffer pre_buffer);

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
    /// Declare one access, returning the layout this call asked the tracker for — which is not always `layout`, since
    /// a texture with a transfer in flight is held at `general` instead.
    /// A recorded copy command names a layout literally, so it has to use what comes back rather than what it wanted.
    /// It is what this call asked for rather than what the box settled on: a second declare against the same box in
    /// one op still goes through `combine_layouts`, which can widen it further, and only the copy paths — one declare,
    /// flushed immediately — use the return value.
    [[nodiscard]] sg::texture_layout track_texture_access(vulkan_texture const& texture,
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

    // Recorded at submit and submitted ahead of _buffer; see vulkan_command_pool::pre_buffer.
    VkCommandBuffer _pre_buffer = VK_NULL_HANDLE;

    /// Textures this list is the first to touch, gathered while recording.
    ///
    /// Tentative on purpose: a concurrently recording list may have touched the same texture first, so this is a
    /// superset of what actually needs an initial transition and never a subset.
    /// Submit claims each one against the texture and drops those another list already took, which is what puts the
    /// transition on the list that runs first rather than the one that recorded first.
    /// Shared rather than raw: `_touched_textures` is cleared before the submit lock, and the claim happens inside it.
    cc::vector<vulkan_texture_handle> _tentative_initial_transitions;

    // Declared for the current op and awaiting the pre-op flush; empty between ops.
    cc::vector<vulkan_buffer const*> _pending_barrier_buffers;
    cc::vector<VkBufferMemoryBarrier2> _pending_buffer_barriers;
    cc::vector<vulkan_texture const*> _pending_barrier_textures;
    cc::vector<VkImageMemoryBarrier2> _pending_image_barriers;

    // The present handshake's half of the submit, set by vulkan_swapchain::record_present_transition.
    //
    // This is where Vulkan needs something from submit that dx12 does not: DXGI gates back-buffer reuse with a fence
    // signaled *after* Present, so its swapchain needs no hook here at all, while vkAcquireNextImageKHR signals a
    // semaphore the first submit must wait on and vkQueuePresentKHR waits on one this submit must signal.
    // Both stay null on an ordinary list, which then submits exactly as before.
    VkSemaphore _present_wait = VK_NULL_HANDLE;
    VkSemaphore _present_signal = VK_NULL_HANDLE;

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

    /// What build_acceleration_structure hands back: the filled build info plus everything the caller has to keep.
    /// A struct rather than out-params, because a build's result is one thing with several parts.
    struct built_acceleration_structure
    {
        VkAccelerationStructureBuildGeometryInfoKHR info = {};
        vulkan_buffer_handle result;
        VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        isize size_in_bytes = 0;
        isize build_scratch_size_in_bytes = 0;
        isize update_scratch_size_in_bytes = 0;
        cc::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    };

    // Sizes the structure, allocates its storage and scratch, creates the object, and declares the result/scratch
    // accesses.
    // Bodies in vulkan_raytracing.cc.
    [[nodiscard]] built_acceleration_structure build_acceleration_structure(
        VkAccelerationStructureTypeKHR type,
        cc::span<VkAccelerationStructureGeometryKHR const> geometries,
        cc::span<u32 const> primitive_counts,
        cc::span<VkAccelerationStructureBuildRangeInfoKHR const> ranges,
        sg::accel_build_flags flags);

    // Flushes what the build declared, then records it.
    void record_acceleration_structure_build(built_acceleration_structure const& built);

    /// Resolves the pending array declares against the bound groups and tracks each named element.
    // Also the accounting pass: a bound array binding with no declaration is an error.
    void declare_array_accesses();

    // Every resource this list has tracked, so submit can finalize each slot and drop can discard it.
    // Public so the context can walk it at submit; deduplicated by mark_recorded.
    //
    // Owning, and that is the point: a transient resource is released the moment its handle leaves scope, which for
    // a per-frame depth buffer is normally before the list is submitted.
    // The per-slot access state lives on the resource, so finalize would then run on a freed object.
    cc::vector<vulkan_buffer_handle> _touched_buffers;
    cc::vector<vulkan_texture_handle> _touched_textures;

protected:
    // Stages the bytes in the context's upload ring and records a copy out of it.
    // Body in vulkan_command_list.cc.
    void transition_texture_layout(sg::raw_texture_handle texture,
                                   sg::texture_layout layout,
                                   cc::optional<sg::subresource_range> const& range) override;

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

    // Ray tracing (reached through cmd.raytracing).
    // is_supported()'s body is in vulkan_command_list.cc, which has vulkan_context complete.
    [[nodiscard]] bool raytracing_is_supported() const override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                  sg::accel_build_flags flags) override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const> geometries,
                                                              sg::accel_build_flags flags) override;
    [[nodiscard]] sg::tlas_handle raytracing_build_tlas(cc::span<sg::tlas_instance const> instances,
                                                        sg::accel_build_flags flags) override;
    void raytracing_bind_pipeline(sg::raytracing_pipeline const& pipeline) override;
    void raytracing_bind_group(int group_index, sg::binding_group const& group) override;
    void raytracing_dispatch_rays(sg::raytracing_shader_table const& table,
                                  sg::raygen_index raygen,
                                  int width,
                                  int height,
                                  int depth) override;

    // GPU queries (reached through cmd.query). Bodies in vulkan_command_list.queries.cc.
    [[nodiscard]] bool query_timestamps_supported() const override;
    [[nodiscard]] sg::gpu_timestamp query_record_gpu_timestamp() override;

public:
    /// Resolves every leased query pool into a transient buffer and starts its readback; runs just before close.
    void finalize_queries_before_close();

    /// Returns the leased pools unresolved; runs on the drop path.
    void release_queries_on_drop();

    /// The query pools this list has leased, and which of them is being filled.
    /// finalize_queries_before_close resolves + reads them back at submit and returns them; a drop returns them
    /// unresolved.
    cc::vector<cc::unique_ptr<vulkan_query_pool_lease>> _leased_query_pools;
    int _active_timestamp_lease = -1;

protected:
};
