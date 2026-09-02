#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_cpu_descriptor_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_download_inline.hh>
#include <shaped-graphics/backends/dx12/dx12_query.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/subresource.hh>

/// One declare_array_buffer_access call, held until the next dispatch resolves it against the bound groups.
struct sg::backend::dx12::dx12_array_buffer_declare
{
    cc::string name;
    cc::vector<sg::array_buffer_access> elements;
};

/// One declare_array_texture_access call, the texture analogue of dx12_array_buffer_declare.
struct sg::backend::dx12::dx12_array_texture_declare
{
    cc::string name;
    cc::vector<sg::array_texture_access> elements;
};

/// DirectX 12 implementation of sg::command_list.
/// Owns its allocator and graphics command list, and is handed out already recording.
/// Buffer and texture transfers stage through the context's inline upload/download systems.
/// Downloads accumulate here token-less and are enqueued when the list is submitted.
class sg::backend::dx12::dx12_command_list final : public sg::command_list
{
public:
    // Defined in the .cc: the sg::command_list base needs dx12_context complete to upcast it to sg::context.
    dx12_command_list(dx12_context& ctx,
                      sg::epoch created_in,
                      sg::command_list_slot slot,
                      D3D12_COMMAND_LIST_TYPE queue,
                      ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList> list,
                      ComPtr<ID3D12CommandAllocator> pre_allocator,
                      ComPtr<ID3D12GraphicsCommandList> pre_list);

    // Auto-drops, with a warning, a list left neither submitted nor dropped.
    // The explicit path is submit_dx12_command_list / drop_dx12_command_list, and this is a no-op once either has run.
    ~dx12_command_list() override;

    /// The access-tracking slot this list holds for its lifetime: it keys the list's private access-state entry in every resource it touches, so concurrent lists never share state.
    [[nodiscard]] sg::command_list_slot slot() const { return _slot; }

    /// Record the barriers collected since the last flush in one `Barrier` call, then clear the pending set.
    /// Those are all the buffer and texture hazards an operation's bound/touched resources implied.
    /// Called just before every GPU op that consumes them, and by nothing else — submit asserts the pending sets are
    /// empty, since its own entry barriers go into the pre-list rather than through here.
    void flush_barriers();

    /// Transition the whole of `texture` to `layout` and record the barrier immediately — declare plus flush.
    /// The new layout becomes the texture's current state when this list submits.
    /// Used by the swapchain to hand a back buffer to Present (sg::texture_layout::present).
    /// The transition is computed from the texture's tracked layout, so it composes with whatever the frame's render pass left it in.
    void transition_texture_to(dx12_texture_handle const& texture, sg::texture_layout layout);

    /// Resolve every leased query heap into one transient buffer and start one inline readback per heap, filling each heap's shared future in place.
    /// Records GPU work, so it must run before Close; submit drives it under the submission lock.
    /// A no-op for a list that recorded no queries.
    void finalize_queries_before_close();

    dx12_context& _ctx;             // creating context — outlives this list
    sg::command_list_slot _slot;    // released to the context's slot allocator on submit/drop
    bool _consumed = false;         // set by submit/drop; gates the destructor's auto-drop
    D3D12_COMMAND_LIST_TYPE _queue; // queue the allocator/list belong to — routes them back to the pool
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;

    // The list executed AHEAD of _list in the same ExecuteCommandLists call, carrying the entry barriers that take
    // each touched resource from what it is really in to what this list's first use of it needs.
    // Acquired with the list rather than at submit, so the submission lock never has to run a fallible acquire.
    ComPtr<ID3D12CommandAllocator> _pre_allocator;
    ComPtr<ID3D12GraphicsCommandList> _pre_list;

    // Deferred readback copies recorded into this list, stamped with the submission token and handed to the download system at submit.
    cc::vector<dx12_download_copy_job> _pending_downloads;

    // Query heaps leased by this list while recording; empty for a list with no queries.
    // Slots are bump-allocated from the active lease.
    // finalize_queries_before_close resolves + reads them back at submit and returns them to the query system; a drop returns them unresolved.
    cc::vector<cc::unique_ptr<dx12_query_heap_lease>> _leased_query_heaps;

    // Index into _leased_query_heaps of the current timestamp heap; -1 means none, or all full.
    // A new heap is leased on demand when this is -1 or the active one is full.
    // One slot per query type — timestamp only, for now.
    int _active_timestamp_lease = -1;

    // Resources whose access has been declared for the *next* GPU op but not yet flushed; empty between ops.
    // track_*_access appends a resource here only on its first binding to the op, so each appears at most once.
    // flush_barriers() flushes each to merge its declares into one barrier, then clears these.
    cc::vector<dx12_buffer_handle> _pending_barrier_buffers;
    cc::vector<dx12_texture_handle> _pending_barrier_textures;

    // Barriers collected for the *next* GPU op; empty between ops.
    // flush_barriers() flushes the pending-barrier resources above into these, then records the whole batch in one Barrier call just before the op.
    // Public so the context can assert at submit that every declared access was flushed by its op.
    cc::vector<D3D12_BUFFER_BARRIER> _pending_buffer_barriers;
    cc::vector<D3D12_TEXTURE_BARRIER> _pending_texture_barriers;

    // Buffers this list has touched: their slots are finalized at submit/drop, and each gets the reverse async-upload stamp at submit.
    cc::vector<dx12_buffer_handle> _touched_buffers;

    // Compute bind state, both reset on compute_bind_pipeline.
    // The bound pipeline layout supplies each slot's root-parameter indices.
    // One bound group per slot, indexed by `set` and sized to the layout's group count, whose views are declared at dispatch.
    dx12_pipeline_layout const* _bound_pipeline_layout = nullptr;
    cc::vector<dx12_binding_group const*> _bound_groups;

    // Array-access declarations for the *next* dispatch (compute + ray tracing share them); cleared after it.
    // Resolved against the bound groups' array_bindings — every bound array binding must be covered by one.
    cc::vector<dx12_array_buffer_declare> _pending_array_buffer_declares;
    cc::vector<dx12_array_texture_declare> _pending_array_texture_declares;

    // Textures this list has touched, so their per-list subresource slots are finalized at submit/drop.
    // A texture finalize can return revert barriers — transitions back to its entry layout on a non-final submit — emitted before Close.
    // Buffers have none.
    cc::vector<dx12_texture_handle> _touched_textures;

    // Raster rendering-scope state; at most one scope is open at a time, since begin/end are balanced.
    // begin_rendering sets _in_render_pass and records the RTV/DSV descriptor slots it created for the pass.
    // end_rendering schedules their epoch-deferred free — they must outlive the list's GPU execution — and clears these.
    bool _in_render_pass = false;
    cc::fixed_vector<cpu_descriptor_slot, sg::max_color_targets> _rendering_rtv_slots;
    cpu_descriptor_slot _rendering_dsv_slot = cpu_descriptor_slot::invalid;

    // Raster (graphics) bind state, separate from the compute/RT state above: graphics binds through a distinct root-signature bind point.
    // That is SetGraphicsRootSignature / SetGraphicsRootDescriptorTable.
    // The bound layout supplies each slot's root-parameter indices; one bound group per slot, declared at draw.
    // Reset on raster_bind_pipeline, and cleared at raster_end_rendering — the bind state is scoped to the pass.
    dx12_pipeline_layout const* _bound_raster_layout = nullptr;
    cc::vector<dx12_binding_group const*> _bound_raster_groups;

    // Vertex / index buffers currently bound to the IA, slot-indexed; a null entry is an unbound slot.
    // Kept so their vertex_read / index_read accesses can be declared for hazard barriers at draw time, the point the GPU reads them.
    // Same rhythm compute uses for its bound groups, and cleared at raster_end_rendering with the rest of the bind state.
    cc::fixed_vector<dx12_buffer_handle, sg::max_vertex_buffers> _bound_vertex_buffers;
    dx12_buffer_handle _bound_index_buffer;

    // The async-upload completions this list must observe: one entry per distinct timeline, at the highest value
    // that timeline owes it.
    // A LIST rather than a single value because completion timelines are per resource — collapsing them to one max
    // would be comparing values from unrelated fences, which is exactly the bug out-of-order selection introduced.
    // At submit the direct queue issues one Wait per entry, so the list sees every async write it touches.
    // Maintained by track_buffer_access / track_texture_access.
    // The reverse stamp — defer a later async upload behind this list — is applied to the touched sets at submit.
    cc::vector<dx12_group_value> _required_copy_waits;

    // The async-download completions any resource this list WRITES must observe, same shape and same reason.
    // Only writes fold in, since two reads never conflict.
    cc::vector<dx12_group_value> _required_download_waits;

protected:
    // Reached through the base's cmd.upload / cmd.download / cmd.copy scopes.
    void transition_texture_layout(sg::raw_texture_handle texture,
                                   sg::texture_layout layout,
                                   cc::optional<sg::subresource_range> const& range) override;

    void upload_bytes_to_buffer(sg::raw_buffer_handle buffer, cc::span<byte const> data, isize offset_in_bytes) override;

    void upload_bytes_to_texture(sg::raw_texture_handle texture,
                                 cc::span<byte const> pixels,
                                 sg::subresource_index const& subresource,
                                 sg::texture_region const& region) override;

    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                              isize offset_in_bytes,
                                                              isize size_in_bytes) override;

    [[nodiscard]] sg::bytes_future download_bytes_from_texture(sg::raw_texture_handle texture,
                                                               sg::subresource_index const& subresource,
                                                               sg::texture_region const& region) override;

    void copy_buffer_region(sg::raw_buffer_handle src,
                            sg::raw_buffer_handle dst,
                            isize src_offset_in_bytes,
                            isize dst_offset_in_bytes,
                            isize size_in_bytes) override;

    // Compute recording (reached through cmd.compute). Bodies in dx12_command_list.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int group_index, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_declare_array_buffer_access(cc::string_view binding_name,
                                             cc::span<sg::array_buffer_access const> elements) override;
    void compute_declare_array_texture_access(cc::string_view binding_name,
                                              cc::span<sg::array_texture_access const> elements) override;
    void compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;

    // Raster rendering scope (reached through cmd.raster). Bodies in dx12_command_list.raster.cc.
    void raster_begin_rendering(sg::rendering_info const& info) override;
    void raster_end_rendering() override;

    // Raster draw recording, reached through cmd.raster / cmd.raster.manual; bodies in dx12_command_list.raster.cc.
    // bind_pipeline binds the graphics root signature + PSO + IA topology.
    // The rest configure IA / dynamic state and record draws through the graphics bind point.
    // Valid only inside an open rendering scope.
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

    // Ray-tracing acceleration-structure builds (reached through cmd.raytracing). Bodies in dx12_raytracing.cc.
    [[nodiscard]] bool raytracing_is_supported() const override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                  sg::accel_build_flags flags) override;
    [[nodiscard]] sg::blas_handle raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const> geometries,
                                                              sg::accel_build_flags flags) override;
    [[nodiscard]] sg::tlas_handle raytracing_build_tlas(cc::span<sg::tlas_instance const> instances,
                                                        sg::accel_build_flags flags) override;

    // Ray-tracing dispatch, reached through cmd.raytracing.
    // Bodies in dx12_command_list.cc, next to the compute equivalents — ray tracing binds through the compute root signature.
    void raytracing_bind_pipeline(sg::raytracing_pipeline const& pipeline) override;
    void raytracing_bind_group(int group_index, sg::binding_group const& group) override;
    void raytracing_dispatch_rays(sg::raytracing_shader_table const& table,
                                  sg::raygen_index raygen,
                                  int width,
                                  int height,
                                  int depth) override;

    // GPU queries (reached through cmd.query). Bodies in dx12_command_list.queries.cc.
    [[nodiscard]] bool query_timestamps_supported() const override;
    [[nodiscard]] sg::gpu_timestamp query_record_gpu_timestamp() override;

private:
    // Declare `stages`/`access` on `buffer` for this list's slot, emit the intra-list barrier the tracker asks for, and record the buffer so its slot is finalized at submit/drop.
    // The barrier is precise (COPY_DEST→COPY_SOURCE and the like), with no bounce through COMMON.
    // Cross-list ordering rides on D3D12's decay of buffers to COMMON at ExecuteCommandLists, so no trailing barrier is needed.
    // Also folds the buffer's pending async-upload value into _required_copy_wait, the forward cross-queue sync for ctx.upload.
    void track_buffer_access(dx12_buffer_handle const& buffer, sg::pipeline_stage_flags stages, sg::access_flags access);

    // Shared BLAS build for both geometry families.
    // Prebuild-query the translated geometry descs, allocate the persistent result + transient scratch, barrier + record the build, and wrap it in a dx12_blas.
    // `input_buffers` are the geometry's build-input buffers, tracked accel_read to order any prior upload.
    [[nodiscard]] sg::blas_handle build_blas_common(cc::span<D3D12_RAYTRACING_GEOMETRY_DESC const> geometry_descs,
                                                    cc::span<dx12_buffer_handle const> input_buffers,
                                                    sg::accel_build_flags flags,
                                                    int geometry_count);

    // Declare `stages`/`access`/`layout` over `range` on `texture` for this list's slot.
    // Emit the per-box layout-transition barriers the tracker asks for, and record the texture so its slot is finalized at submit/drop.
    // Called before every op that reads or writes a texture: transfers, copies, compute and ray-tracing dispatches, draws, and the rendering scope's target transitions.
    void track_texture_access(dx12_texture_handle const& texture,
                              sg::subresource_range range,
                              sg::pipeline_stage_flags stages,
                              sg::access_flags access,
                              sg::texture_layout layout);

    // Declare the hazard accesses a draw consumes before flushing: each bound group's buffer/texture views, the bound vertex buffers, and when `indexed` the index buffer.
    // Called by raster_draw / raster_draw_indexed just before flush_barriers and the draw.
    void declare_raster_draw_barriers(bool indexed);

    // Resolve the pending array-access declarations against the bound groups' array bindings and track each
    // declared element's access, then clear the pending set.
    // Asserts every bound array binding is covered by a declaration, and every declaration names a bound one.
    // Called by compute_dispatch / raytracing_dispatch_rays alongside the scalar hazard declares.
    void declare_array_accesses();
};
