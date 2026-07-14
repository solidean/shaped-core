#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backend/command_list_slot.hh>
#include <shaped-graphics/backend/subresource.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_cpu_descriptor_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_download_inline.hh>
#include <shaped-graphics/backends/dx12/dx12_query.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::command_list. Owns its allocator and graphics command list,
/// handed out already recording. Buffer transfers stage through the context's inline upload/download
/// systems; downloads accumulate here token-less and are enqueued when the list is submitted.
class dx12_command_list final : public sg::command_list
{
public:
    dx12_command_list(dx12_context& ctx,
                      sg::epoch created_in,
                      sg::command_list_slot slot,
                      D3D12_COMMAND_LIST_TYPE queue,
                      ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList> list)
      : sg::command_list(created_in),
        _ctx(ctx),
        _slot(slot),
        _queue(queue),
        _allocator(cc::move(allocator)),
        _list(cc::move(list))
    {
    }

    // Auto-drops (with a warning) a list left neither submitted nor dropped — the explicit path is
    // submit_dx12_command_list / drop_dx12_command_list. No-op once either has run (they mark it consumed).
    ~dx12_command_list() override;

    /// The access-tracking slot this list holds for its lifetime — keys its private access-state entry in
    /// every resource it touches, so concurrent lists don't share state (a backend helper, not sg API).
    [[nodiscard]] sg::command_list_slot slot() const { return _slot; }

    /// Record the barriers collected since the last flush (all the buffer + texture hazards an operation's
    /// bound/touched resources implied), in one `Barrier` call, then clear the pending set. Called just
    /// before every GPU op that consumes them, and by the context at submit for the finalize reverts.
    void flush_barriers();

    /// Resolve every leased query heap into one transient buffer and start one inline readback per heap,
    /// filling each heap's shared future in place. Records GPU work, so it must run before Close and is
    /// driven from submit under the submission lock. No-op for a list that recorded no queries.
    void finalize_queries_before_close();

    dx12_context& _ctx;             // creating context — outlives this list
    sg::command_list_slot _slot;    // released to the context's slot allocator on submit/drop
    bool _consumed = false;         // set by submit/drop; gates the destructor's auto-drop
    D3D12_COMMAND_LIST_TYPE _queue; // queue the allocator/list belong to — routes them back to the pool
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;

    // Deferred readback copies recorded into this list; stamped with the submission token and handed
    // to the download system at submit (empty for a list with no downloads).
    cc::vector<dx12_download_copy_job> _pending_downloads;

    // Query heaps leased by this list while recording (empty for a list with no queries). Slots are
    // bump-allocated from the active lease; finalize_queries_before_close resolves + reads them back at
    // submit and returns them to the query system. A drop returns them unresolved.
    cc::vector<cc::unique_ptr<dx12_query_heap_lease>> _leased_query_heaps;

    // Index into _leased_query_heaps of the current timestamp heap (-1 = none / all full). A new heap is
    // leased on demand when this is -1 or the active one is full. One slot per query type (timestamp only).
    int _active_timestamp_lease = -1;

    // Resources whose access has been declared for the *next* GPU op but not yet flushed. track_*_access
    // appends a resource here on its first binding to the op only (declare reports that), so each appears at
    // most once; flush_barriers() flushes each to merge its declares into one barrier, then clears these.
    // Empty between ops.
    cc::vector<dx12_buffer_handle> _pending_barrier_buffers;
    cc::vector<dx12_texture_handle> _pending_barrier_textures;

    // Barriers collected for the *next* GPU op: flush_barriers() flushes the pending-barrier resources above
    // into these, then records the whole batch in one Barrier call just before the op. Empty between ops.
    // Public so the context can stage the finalize reverts here at submit.
    cc::vector<D3D12_BUFFER_BARRIER> _pending_buffer_barriers;
    cc::vector<D3D12_TEXTURE_BARRIER> _pending_texture_barriers;

    // Access tracking: buffers this list has touched (so their slots are finalized at submit/drop, and so
    // each gets the reverse async-upload stamp at submit).
    cc::vector<dx12_buffer_handle> _touched_buffers;

    // Compute bind state: the bound pipeline layout supplies each slot's root-parameter indices, and one
    // bound group per slot (indexed by `set`, sized to the layout's group count) whose views are declared
    // at dispatch. Both reset on compute_bind_pipeline.
    dx12_pipeline_layout const* _bound_pipeline_layout = nullptr;
    cc::vector<dx12_binding_group const*> _bound_groups;

    // Textures this list has touched, so their per-list subresource slots are finalized at submit/drop. A
    // texture finalize can return revert barriers (transitions back to its entry layout on a non-final
    // submit) that are emitted before Close — unlike buffers, which are teeth-free.
    cc::vector<dx12_texture_handle> _touched_textures;

    // Raster rendering-scope state. begin_rendering sets _in_render_pass and records the RTV/DSV descriptor
    // slots it created for the pass; end_rendering schedules their epoch-deferred free (they must outlive the
    // list's GPU execution) and clears these. At most one scope is open at a time (begin/end are balanced).
    bool _in_render_pass = false;
    cc::fixed_vector<cpu_descriptor_slot, sg::max_color_targets> _rendering_rtv_slots;
    cpu_descriptor_slot _rendering_dsv_slot = cpu_descriptor_slot::invalid;

    // Raster (graphics) bind state — separate from the compute/RT bind state above because graphics uses a
    // distinct root-signature bind point (SetGraphicsRootSignature / SetGraphicsRootDescriptorTable). The
    // bound layout supplies each slot's root-parameter indices; one bound group per slot, declared at draw.
    // Both reset on raster_bind_pipeline.
    dx12_pipeline_layout const* _bound_raster_layout = nullptr;
    cc::vector<dx12_binding_group const*> _bound_raster_groups;

    // Vertex / index buffers currently bound to the IA (slot-indexed; null entries are unbound slots). Kept
    // so their vertex_read / index_read accesses can be declared for hazard barriers at draw time (the
    // point the GPU reads them), the same rhythm compute uses for its bound groups.
    cc::fixed_vector<dx12_buffer_handle, sg::max_vertex_buffers> _bound_vertex_buffers;
    dx12_buffer_handle _bound_index_buffer;

    // Highest async-upload completion value any buffer this list touches is waiting on. At submit the
    // direct queue waits on the copy fence for this value, so the list sees the async writes. `none`
    // means no touched buffer had a pending async upload. Maintained by track_buffer_access; the reverse
    // stamp (defer a later async upload behind this list) is applied to _touched_buffers at submit.
    dx12_copy_fence_value _required_copy_wait = dx12_copy_fence_value::none;

    // Highest async-download completion value any buffer this list WRITES is waiting on. At submit the
    // direct queue waits on the download fence for this value, so the write never overwrites bytes an async
    // readback is still reading. `none` means no written buffer had a pending async download. Only writes
    // fold in (two reads don't conflict). Maintained by track_buffer_access.
    dx12_download_fence_value _required_download_wait = dx12_download_fence_value::none;

protected:
    // Reached through the base's cmd.upload / cmd.download / cmd.copy scopes.
    void upload_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                cc::span<cc::byte const> data,
                                cc::isize offset_in_bytes) override;

    void upload_bytes_to_texture(sg::raw_texture_handle texture,
                                 cc::span<cc::byte const> pixels,
                                 sg::subresource_index const& subresource,
                                 sg::texture_region const& region) override;

    [[nodiscard]] sg::bytes_future download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                              cc::isize offset_in_bytes,
                                                              cc::isize size_in_bytes) override;

    [[nodiscard]] sg::bytes_future download_bytes_from_texture(sg::raw_texture_handle texture,
                                                               sg::subresource_index const& subresource,
                                                               sg::texture_region const& region) override;

    void copy_buffer_region(sg::raw_buffer_handle src,
                            sg::raw_buffer_handle dst,
                            cc::isize src_offset_in_bytes,
                            cc::isize dst_offset_in_bytes,
                            cc::isize size_in_bytes) override;

    // Compute recording (reached through cmd.compute). Bodies in dx12_command_list.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int set, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_declare_array_buffer_access(cc::string_view binding_name,
                                             cc::span<sg::array_buffer_access const> elements) override;
    void compute_declare_array_texture_access(cc::string_view binding_name,
                                              cc::span<sg::array_texture_access const> elements) override;
    void compute_set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset) override;

    // Raster rendering scope (reached through cmd.raster). Bodies in dx12_command_list.raster.cc.
    void raster_begin_rendering(sg::rendering_info const& info) override;
    void raster_end_rendering() override;

    // Raster draw recording (reached through cmd.raster / cmd.raster.manual). Bodies in dx12_command_list.raster.cc.
    // bind_pipeline binds the graphics root signature + PSO + IA topology; the rest configure IA / dynamic
    // state and record draws through the graphics bind point. Valid only inside an open rendering scope.
    void raster_bind_pipeline(sg::raster_pipeline const& pipeline) override;
    void raster_bind_group(int set, sg::binding_group const& group) override;
    void raster_bind_vertex_buffers(int first_slot, cc::span<sg::vertex_buffer_view const> views) override;
    void raster_bind_index_buffer(sg::index_buffer_view const& view) override;
    void raster_set_viewport(sg::viewport const& vp) override;
    void raster_set_scissor(tg::aabb2i const& rect) override;
    void raster_set_stencil_reference(sg::u32 reference) override;
    void raster_set_blend_constants(tg::vec4f constants) override;
    void raster_set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset) override;
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

    // Ray-tracing dispatch (reached through cmd.raytracing). Bodies in dx12_command_list.cc, next to the
    // compute equivalents (ray tracing binds through the compute root signature).
    void raytracing_bind_pipeline(sg::raytracing_pipeline const& pipeline) override;
    void raytracing_bind_group(int set, sg::binding_group const& group) override;
    void raytracing_dispatch_rays(sg::raytracing_shader_table const& table,
                                  sg::raygen_index raygen,
                                  int width,
                                  int height,
                                  int depth) override;

    // GPU queries (reached through cmd.query). Bodies in dx12_command_list.queries.cc.
    [[nodiscard]] bool query_timestamps_supported() const override;
    [[nodiscard]] sg::gpu_timestamp query_record_gpu_timestamp() override;

private:
    // Declare `stages`/`access` on `buffer` for this list's slot, emit the intra-list barrier the tracker
    // asks for (COPY_DEST→COPY_SOURCE and the like — precise, no bounce through COMMON), and record the
    // buffer so its slot is finalized at submit/drop. Cross-list ordering rides on D3D12's decay of buffers
    // to COMMON at ExecuteCommandLists, so no trailing barrier is needed. Also folds the buffer's pending
    // async-upload value into _required_copy_wait (the forward cross-queue sync for ctx.upload).
    void track_buffer_access(dx12_buffer_handle const& buffer, sg::pipeline_stage_flags stages, sg::access_flags access);

    // Shared BLAS build for both geometry families: prebuild-query the translated geometry descs, allocate the
    // persistent result + transient scratch, barrier + record the build, and wrap it in a dx12_blas.
    // `input_buffers` are the geometry's build-input buffers (tracked accel_read to order any prior upload).
    [[nodiscard]] sg::blas_handle build_blas_common(cc::span<D3D12_RAYTRACING_GEOMETRY_DESC const> geometry_descs,
                                                    cc::span<dx12_buffer_handle const> input_buffers,
                                                    sg::accel_build_flags flags,
                                                    int geometry_count);

    // Declare `stages`/`access`/`layout` over `range` on `texture` for this list's slot, emit the per-box
    // layout-transition barriers the tracker asks for, and record the texture so its slot is finalized at
    // submit/drop. Driven by download_bytes_from_texture and the raster rendering scope (render-target /
    // depth-stencil transitions); future texture copy / upload / dispatch ops will use it too.
    void track_texture_access(dx12_texture_handle const& texture,
                              sg::subresource_range range,
                              sg::pipeline_stage_flags stages,
                              sg::access_flags access,
                              sg::texture_layout layout);

    // Declare the hazard accesses a draw consumes before flushing: each bound group's buffer/texture views
    // (like compute_dispatch) plus the bound vertex buffers and, when `indexed`, the index buffer. Called
    // by raster_draw / raster_draw_indexed just before flush_barriers + the draw.
    void declare_raster_draw_barriers(bool indexed);
};
} // namespace sg::backend::dx12
