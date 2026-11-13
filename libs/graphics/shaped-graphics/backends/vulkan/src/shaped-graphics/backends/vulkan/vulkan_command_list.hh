#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_inline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_query.hh>
#include <shaped-graphics/barrier/array_declarations.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/reserved_bindings.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>

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
    /// An access landing in the write bucket of an array member counts toward _member_writes.
    /// `from_array_application` makes the buffer one, and the result says whether the op already held a write of it.
    /// The context's debug window for the epoch being recorded, where diagnostics are on; null where they are off.
    /// Every op that runs with it bound declares it as an append (declare_debug_window), as a bound group is declared.
    [[nodiscard]] sg::raw_buffer_handle bind_debug_window(sg::impl::pipeline_footprint const& footprint);
    void declare_debug_window(sg::impl::pipeline_footprint const* footprint, sg::pipeline_stage_flags op_stages);
    sg::raw_buffer_handle _bound_debug_window;

    sg::impl::member_track track_buffer_access(vulkan_buffer const& buffer,
                                               sg::pipeline_stage_flags stages,
                                               sg::access_flags access,
                                               bool from_array_application = false);

    /// One polyfilled mesh draw, of the grid `x, y, z`, or of the record of `dispatch_args` at `offset_in_bytes` of
    /// `args` where it is non-null.
    void draw_mesh_polyfilled(sg::impl::mesh_polyfill_state const& polyfill,
                              cc::fixed_array<int, 3> grid,
                              std::shared_ptr<vulkan_buffer const> const& args,
                              isize offset_in_bytes);

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

    /// What `track_texture` found: the layout asked for, as `track_texture_access` returns it, and the array-member report.
    struct texture_track
    {
        sg::texture_layout layout = sg::texture_layout::general;
        sg::impl::member_track found;
    };

    /// `track_texture_access`, counting toward _member_writes as the buffer form does.
    [[nodiscard]] texture_track track_texture(vulkan_texture const& texture,
                                              sg::subresource_range range,
                                              sg::pipeline_stage_flags stages,
                                              sg::access_flags access,
                                              sg::texture_layout layout,
                                              bool from_array_application);

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

    // Buffers of an array a dispatch or draw declared nothing for, whose barriers this op folds into one memory barrier.
    // Their tracked state still moves per buffer; only the emission is shared.
    cc::set<vulkan_buffer const*> _global_barrier_buffers;
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
    /// The bound compute pipeline, which a polyfilled mesh draw binds again after its own compute half.
    VkPipeline _bound_compute_pipeline = VK_NULL_HANDLE;
    cc::vector<vulkan_binding_group const*> _bound_groups;
    // The bound compute or raytracing pipeline's footprint, which the dispatch declares its groups' accesses from.
    // The pipeline itself is kept alive by keep_bound.
    sg::impl::pipeline_footprint const* _bound_footprint = nullptr;
    void const* _bound_footprint_owner = nullptr; // the pipeline, which a footprint mismatch is logged against

    /// What was bound, held until the list is consumed: a group or pipeline dropped between its bind and the draw that reads it must still be there.
    cc::vector<std::shared_ptr<void const>> _bound_keep_alive;

    void keep_bound(std::shared_ptr<void const> object)
    {
        if (object != nullptr && (_bound_keep_alive.empty() || _bound_keep_alive.back() != object))
            _bound_keep_alive.push_back(cc::move(object));
    }

    // Array bindings are not auto-tracked, so their accesses arrive as explicit declarations, in force until withdrawn.
    // The compute bind point's, which ray tracing shares.
    sg::impl::array_declaration_book _compute_arrays;

    // How many accesses this list tracked into the write bucket of an array member, which ends every current application.
    u64 _member_writes = 0;

    // Armed by an op whose arrays were skipped, and checked by the next flush_barriers.
    sg::impl::array_skip_guard _skip_guard;

    // The bound arrays of the op being recorded, kept to reuse their storage.
    cc::vector<sg::impl::array_site<vulkan_array_binding>> _array_sites;

    // Set inside a rendering scope; a dispatch inside one is rejected because Vulkan forbids it outright.
    bool _in_render_pass = false;

    // Attachment views this list's scopes created, handed to the epoch's deferred deletion at submit and at drop alike.
    // A dropped list's views go to the epoch too: a texture caches them, so another list of the epoch may use them.
    cc::vector<VkImageView> _created_attachment_views;

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
    /// The layers every target of the scope spans, which a vertex stage's `@layer` picks among.
    u32 _rendering_layer_count = 1;
    u32 _rendering_view_mask = 0; // nonzero for a rendering of several views; a reopen repeats it

    // Opens the rendering instance from the stored attachments, forcing every load op to LOAD.
    // Body in vulkan_command_list.raster.cc.
    void reopen_rendering();

    // A copy is as illegal inside a dynamic-rendering instance as a barrier (VUID-vkCmdCopyBuffer-renderpass).
    // sg allows either in a rendering scope, so the instance is suspended: ended, and left ended until a draw needs it.
    // Several uploads in a row then cost one split rather than one each.
    // Bodies in vulkan_command_list.raster.cc.
    void suspend_rendering(cc::string_view cause, split_remedy remedy = split_remedy::record_outside_scope);
    void resume_rendering();
    bool _rendering_suspended = false;

    // What ended the open scope's pass, and what to do about it, named when its next draw reopens it.
    cc::string_view _rendering_split_cause;
    split_remedy _rendering_split_remedy = split_remedy::record_outside_scope;

    // The graphics bind + input-assembly state, all scoped to the rendering scope that set it up.
    vulkan_pipeline_layout const* _bound_raster_layout = nullptr;
    cc::vector<vulkan_binding_group const*> _bound_raster_groups;
    sg::impl::pipeline_footprint const* _bound_raster_footprint = nullptr;
    void const* _bound_raster_footprint_owner = nullptr;
    sg::impl::array_declaration_book _raster_arrays; // ended with the rendering scope
    cc::vector<vulkan_buffer const*> _bound_vertex_buffers;
    vulkan_buffer const* _bound_index_buffer = nullptr;

    // What the bound index buffer contributes to the alignment rule an indexed draw must satisfy.
    // See sg::index_buffer_offset_alignment; every backend carries this check.
    sg::index_format _index_format = sg::index_format::uint16;
    isize _index_view_offset_in_bytes = 0;

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

    /// Meets one bind point's live array declarations with its bound groups and `footprint`, and tracks what
    /// sg::impl::array_declaration_book::apply decides at `op_stages`.
    /// A mismatch is logged against `pipeline`.
    void declare_array_accesses(cc::span<vulkan_binding_group const* const> groups,
                                sg::impl::pipeline_footprint const* footprint,
                                void const* pipeline,
                                sg::pipeline_stage_flags op_stages,
                                sg::impl::array_declaration_book& book);

    /// Declares every bound group's views at an op of `op_stages`, as `footprint` says the code touches them.
    /// Binds set 3 of a layout traced in software, right before an op runs.
    void bind_software_trace(vulkan_pipeline_layout const& layout,
                             cc::span<vulkan_binding_group const* const> groups,
                             VkPipelineBindPoint bind_point);
    vulkan_pipeline_layout const* _software_set_layout = nullptr;
    u64 _software_set_pool = 0;
    u32 _software_set_roots[16] = {};
    u32 _software_set_grid[4] = {};
    u32 _software_set_addresses[2 * sg::impl::acceleration_address_capacity] = {};
    u32 _software_set_lengths[16] = {};
    VkDeviceAddress _software_set_mesh_slots = 0;
    VkDeviceAddress _software_set_task_slots = 0;
    /// The slot buffer of the polyfilled mesh draw being recorded, which set 3 names where the layout holds its slots.
    VkDeviceAddress _mesh_slots = 0;
    isize _mesh_slots_bytes = 0;
    /// The record buffer of its task stage, likewise.
    VkDeviceAddress _task_slots = 0;
    isize _task_slots_bytes = 0;

    void declare_group_accesses(cc::span<vulkan_binding_group const* const> groups,
                                sg::impl::pipeline_footprint const* footprint,
                                sg::pipeline_stage_flags op_stages);

    // Every resource this list has tracked, so submit can finalize each slot and drop can discard it.
    // Public so the context can walk it at submit; deduplicated by mark_recorded.
    //
    // Owning, because a transient resource is released the moment its handle leaves scope, which for
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

    void copy_buffer_to_texture(sg::raw_buffer_handle src,
                                isize src_offset_in_bytes,
                                isize src_row_pitch_in_bytes,
                                sg::raw_texture_handle dst,
                                sg::subresource_index const& subresource,
                                sg::texture_region const& region) override;

    // Compute recording (reached through cmd.compute). Bodies in vulkan_command_list.compute.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int group_index, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_dispatch_indirect(sg::raw_buffer_handle const& args, isize offset_in_bytes) override;
    [[nodiscard]] u32 compute_declare_array_buffer_access(cc::string_view binding_name,
                                                          cc::span<sg::array_buffer_access const> elements) override;
    [[nodiscard]] u32 compute_declare_array_texture_access(cc::string_view binding_name,
                                                           cc::span<sg::array_texture_access const> elements) override;
    void withdraw_array_declaration(u32 id) override;
    void compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;

    // Raster rendering scope + draws (reached through cmd.raster / cmd.raster.manual).
    // Bodies in vulkan_command_list.raster.cc.
    void raster_begin_rendering(sg::rendering_info const& info) override;
    void raster_end_rendering() override;
    void raster_bind_pipeline(sg::raster_pipeline const& pipeline) override;
    void raster_bind_group(int group_index, sg::binding_group const& group) override;
    void raster_bind_vertex_buffers(int first_slot, cc::span<sg::vertex_buffer_view const> views) override;
    void raster_bind_index_buffer(sg::index_buffer_view const& view) override;
    [[nodiscard]] u32 raster_declare_array_buffer_access(cc::string_view binding_name,
                                                         cc::span<sg::array_buffer_access const> elements) override;
    [[nodiscard]] u32 raster_declare_array_texture_access(cc::string_view binding_name,
                                                          cc::span<sg::array_texture_access const> elements) override;
    void raster_set_viewport(sg::viewport const& vp) override;
    void raster_set_scissor(tg::aabb2i const& rect) override;
    void raster_set_stencil_reference(u32 reference) override;
    void raster_set_blend_constants(tg::vec4f constants) override;
    void raster_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;
    void raster_draw(sg::draw_config const& config) override;
    void raster_draw_mesh_tasks(int x, int y, int z) override;
    void raster_draw_indexed(sg::draw_indexed_config const& config) override;
    void raster_draw_indexed_indirect(sg::raw_buffer_handle const& args, isize offset_in_bytes, isize count) override;
    void raster_draw_mesh_tasks_indirect(sg::raw_buffer_handle const& args, isize offset_in_bytes, isize count) override;
    void raster_draw_mesh_polyfilled(sg::impl::mesh_polyfill_state const& polyfill, int x, int y, int z) override;
    void raster_draw_mesh_polyfilled_indirect(sg::impl::mesh_polyfill_state const& polyfill,
                                              sg::raw_buffer_handle const& args,
                                              isize offset_in_bytes,
                                              isize count) override;

    // Ray tracing (reached through cmd.raytracing).
    // is_supported()'s body is in vulkan_command_list.cc, which has vulkan_context complete.
    [[nodiscard]] bool raytracing_is_supported() const override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                  sg::accel_build_flags flags,
                                                                  int hit_record_stride) override;
    [[nodiscard]] sg::micromap_handle raytracing_build_micromap(sg::micromap_triangles const& triangles) override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const> geometries,
                                                              sg::accel_build_flags flags,
                                                              int hit_record_stride) override;
    [[nodiscard]] sg::tlas_handle raytracing_build_tlas(cc::span<sg::tlas_instance const> instances,
                                                        sg::accel_build_flags flags) override;
    void raytracing_update_partitioned_tlas(sg::partitioned_tlas const& ptlas,
                                            sg::ptlas_operations const& ops,
                                            bool is_first,
                                            cc::vector<sg::blas_handle>&& released) override;
    [[nodiscard]] sg::cluster_set_handle raytracing_build_clusters(cc::span<sg::cluster_triangles const> clusters,
                                                                   sg::accel_build_flags flags) override;
    void raytracing_build_clusters_indirect(sg::cluster_pool const& pool,
                                            sg::cluster_indirect_build const& build) override;
    [[nodiscard]] cc::vector<sg::blas_handle> raytracing_build_cluster_blas(sg::cluster_set_handle const& clusters,
                                                                            cc::span<sg::cluster_range const> ranges,
                                                                            sg::accel_build_flags flags,
                                                                            int hit_record_stride) override;
    void raytracing_update_partitioned_tlas_indirect(sg::partitioned_tlas const& ptlas,
                                                     sg::ptlas_indirect_operations const& ops,
                                                     bool is_first,
                                                     cc::vector<sg::blas_table_handle>&& released) override;
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
