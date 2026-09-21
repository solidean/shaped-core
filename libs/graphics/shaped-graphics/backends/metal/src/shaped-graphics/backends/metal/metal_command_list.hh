#pragma once

// metal-cpp's umbrella Metal.hpp does not include this one, and nothing else in the package does either — so the
// MTL4 acceleration-structure descriptors are unreachable without naming it, unlike every other MTL4 type.
#include <Metal/MTL4AccelerationStructure.hpp>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_barrier.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_query.hh>
#include <shaped-graphics/backends/metal/metal_staging_ring.hh>
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
    ///
    /// `will_submit` is false on the drop path, where the buffer is closed only to be thrown away: a resolve recorded
    /// into it would never run, and it would take a staging reservation with it.
    void end_recording(bool will_submit);

    /// One download this list recorded: the copy out of staging, plus what settles when it runs — or when it never
    /// does.
    ///
    /// **The staging bookkeeping is here rather than inside the closure** because it has to happen on both paths.
    /// A dropped list cancels the future and still owes the ring its count and the overflow buffer its release.
    struct pending_download
    {
        cc::unique_function<void()> copy_out; ///< memcpy out of staging and settle `completion`
        cc::shared_async<cc::unit> completion;

        /// Non-null when this download overflowed the ring: a retain of its own, so the epoch's release cannot free
        /// the bytes before the copy out reads them.
        MTL::Buffer* staging_retained = nullptr;

        /// Non-null when the bytes came from the ring: the epoch's copy count, which holds its span until this runs.
        cc::shared_ptr<cc::atomic<int>> ring_copy;

        /// Releases what the copy out borrowed, whether it ran or was cancelled.
        void settle_staging();
    };

    /// One acceleration structure a declare named, kept alive for as long as the recording that names it.
    /// Type-erased owner because sg::blas and sg::tlas share no base, while the tracking lives on the storage both hold.
    struct accel_declare
    {
        std::shared_ptr<void const> owner;
        metal_accel_storage const* storage = nullptr;
    };

    /// This list's slot in every resource's concurrent access tracking.
    [[nodiscard]] sg::command_list_slot slot() const { return _slot; }

    /// The buffers this list declared against, each needing a finalize at submit or a discard at drop.
    [[nodiscard]] cc::span<sg::raw_buffer_handle const> touched_buffers() const { return _touched_buffers; }

    /// The textures this list declared against; the same contract as touched_buffers.
    [[nodiscard]] cc::span<sg::raw_texture_handle const> touched_textures() const { return _touched_textures; }

    /// The acceleration structures this list declared against; the same contract again.
    /// No transfer ever targets one, so unlike the two above these need no stream wait and no pending-transfer query.
    [[nodiscard]] cc::span<accel_declare const> touched_accels() const { return _touched_accels; }

    /// The copy-outs this list's downloads are waiting on, handed to the submit that will run them.
    /// Moved out, so the list keeps none afterwards.
    [[nodiscard]] cc::vector<pending_download> take_pending_downloads() { return cc::move(_pending_downloads); }

    /// Undoes everything a list that will never run left behind: the per-slot access state on every resource it
    /// declared against, and its downloads, cancelled rather than left unsettled.
    /// Shared by `drop_command_list` and the destructor's rescue path, which is what keeps the two from drifting.
    void abandon_recording();

    /// Hands the allocator and the buffer over; the list owns neither afterwards.
    /// Called by the context once it has taken responsibility for them, whether the list is submitted or dropped.
    void release_ownership();

    /// Hands the argument table over, or null if this list never bound anything.
    /// The GPU reads it while the command buffer runs, so a submitted list's table goes back with the epoch and a
    /// dropped one's goes back at once.
    [[nodiscard]] MTL4::ArgumentTable* take_argument_table()
    {
        auto* const table = _argument_table;
        _argument_table = nullptr;
        return table;
    }

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
    void raster_declare_array_buffer_access(cc::string_view binding_name,
                                            cc::span<array_buffer_access const> elements) override;
    void raster_declare_array_texture_access(cc::string_view binding_name,
                                             cc::span<array_texture_access const> elements) override;
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

    /// Resolves every leased counter heap into the download ring and starts one readback per heap.
    /// Called from end_recording, after the last encoder closed: a resolve is a command-buffer-level call, and it has
    /// to follow everything it measures.
    void finalize_queries_before_close();

    /// Hands every leased heap back unresolved.
    /// A dropped list never runs, so each handle keeps its invalid future — which is what "never ready" means for a
    /// timestamp whose list was dropped.
    void release_queries_on_drop();

    /// The encoder every copy and dispatch records into, opened on first use.
    ///
    /// **Metal 4 has no blit encoder.** Its compute encoder carries `copyFromBuffer`, `copyFromTexture` and
    /// `fillBuffer` alongside dispatch, where dx12 and vulkan each have a distinct copy path — so one encoder serves
    /// both, and a barrier on it may name `MTLStageBlit` and `MTLStageDispatch` alike.
    [[nodiscard]] MTL4::ComputeCommandEncoder* compute_encoder();

    /// Closes the open encoder, if any.
    /// A render pass needs the compute one closed first.
    void end_encoder();

    /// The render encoder of the open rendering scope; null outside one.
    [[nodiscard]] MTL4::RenderCommandEncoder* render_encoder() const { return _render_encoder; }

    /// Declare access on everything the bound groups name, and flush — the shape a draw and a dispatch share.
    void declare_bound_groups(pipeline_stage_flags stages);

    /// Forgets which resources each slot's group named, so the next dispatch or draw declares only what it binds.
    ///
    /// Called from every bind_pipeline, which is where sg's bound groups reset — dx12 and vulkan resize their
    /// bound-group vector there for the same reason.
    /// Without it one argument table is shared across bind points with no reset anywhere, so a pass would declare a
    /// group an earlier dispatch bound — and if that group named this pass's render target, declare shader access on
    /// a texture the output-merger holds.
    void reset_bound_group_tracking();

    /// Write a group's argument-buffer address into the table and remember what it names.
    /// Shared by the compute and raster bind paths, which differ only in which encoder is open.
    void bind_group_to_table(int group_index, binding_group const& group);

    /// Declare `access` on `buffer` for the op about to be recorded, and remember it for the finalize at submit.
    void declare_buffer(raw_buffer_handle const& buffer, pipeline_stage_flags stages, access_flags access);

    /// The texture twin of declare_buffer.
    /// A texture carries no layout here, so the two differ only in which list the resource is remembered on.
    void declare_texture(raw_texture_handle const& texture, pipeline_stage_flags stages, access_flags access);

    /// The acceleration-structure twin, for an AS build and for a trace against a bound TLAS.
    /// `owner` keeps the structure alive and is type-erased because sg::blas and sg::tlas share no base, while the
    /// tracking itself lives on the storage they both hold.
    void declare_accel(std::shared_ptr<void const> owner,
                       metal_accel_storage const& storage,
                       pipeline_stage_flags stages,
                       access_flags access);

    /// Take ownership of a staging reservation that got a buffer of its own, making it resident and freeing it with
    /// the epoch.
    /// A no-op for a reservation that came out of the ring.
    void adopt_overflow_staging(metal_staging_ring::reservation const& staging);

    /// Size an acceleration structure from its descriptor, mint it, and declare it resident.
    /// The three sizes come back through the out-params because Metal answers all of them in one query.
    [[nodiscard]] MTL::AccelerationStructure* build_accel_common(MTL4::AccelerationStructureDescriptor* descriptor,
                                                                 isize& out_size,
                                                                 isize& out_build_scratch,
                                                                 isize& out_update_scratch);

    /// The half a triangle BLAS and a procedural one share, once their geometry descriptors are built.
    [[nodiscard]] blas_handle build_blas_common(MTL4::PrimitiveAccelerationStructureDescriptor* descriptor,
                                                cc::span<raw_buffer_handle const> input_buffers,
                                                accel_build_flags flags,
                                                int geometry_count);

    /// A retain on an overflow download's staging buffer, or null for a reservation inside the ring.
    [[nodiscard]] static MTL::Buffer* retain_download_staging(metal_staging_ring::reservation const& staging);

    /// The open epoch's copy count for a download staged inside the ring, or null for an overflow reservation.
    [[nodiscard]] cc::shared_ptr<cc::atomic<int>> account_download_staging(metal_staging_ring::reservation const& staging);

    /// Emit the barriers every buffer declared since the last flush needs, then clear the declares.
    /// Called immediately before the op those declares were for.
    void flush_barriers();

    /// The argument table every bound group is written into, created on first use.
    ///
    /// **MTL4 binds through a table rather than per-encoder setters.** One table serves this list, and a group bound
    /// at slot N writes its argument buffer's address into buffer-binding N — which is what makes sg's group_index the
    /// MSL `[[buffer(N)]]` index directly.
    [[nodiscard]] MTL4::ArgumentTable* argument_table();

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

    cc::vector<sg::raw_texture_handle> _touched_textures;
    cc::vector<sg::raw_texture_handle> _pending_textures;

    cc::vector<accel_declare> _touched_accels;
    cc::vector<accel_declare> _pending_accels;

    MTL4::ArgumentTable* _argument_table = nullptr;
    MTL4::RenderCommandEncoder* _render_encoder = nullptr;

    /// The pipeline of the open rendering scope, for the primitive type a draw is issued with.
    metal_raster_pipeline const* _bound_raster = nullptr;

    /// The pipeline currently bound, for the workgroup size a thread-count dispatch divides by.
    metal_compute_pipeline const* _bound_compute = nullptr;

    /// The open rendering scope's depth-stencil target format, `undefined` for none.
    /// What a bound pipeline's own `depth_stencil_format` is checked against, since MTL4 builds the pipeline without
    /// it and would otherwise never notice a mismatch.
    sg::pixel_format _scope_depth_stencil_format = sg::pixel_format::undefined;

    /// The layout of whichever pipeline was bound last, and what every group bound after it is checked against.
    /// One member for all three kinds: a bind replaces it, which is the same rule the encoders follow.
    metal_pipeline_layout const* _bound_layout = nullptr;

    /// The ray-tracing pipeline a dispatch_rays must have had its table built for.
    /// Nothing is bound to an encoder at bind time: which compute state runs is decided by the raygen the dispatch
    /// names, so this is the check rather than the binding.
    metal_raytracing_pipeline const* _bound_raytracing = nullptr;

    /// Per slot, the buffers the group bound there names — copied at bind time, because a binding_group is handed over
    /// by reference and has no handle to take.
    /// Rebinding a slot replaces its list, and `reset_bound_group_tracking` empties every slot when a pipeline is
    /// bound, so what a dispatch or draw declares is exactly what is bound when it runs.
    cc::vector<sg::raw_buffer_handle> _group_buffers[sg::max_binding_groups];
    cc::vector<sg::raw_texture_handle> _group_textures[sg::max_binding_groups];
    cc::vector<sg::tlas_handle> _group_tlases[sg::max_binding_groups];

    /// One per download recorded: copies the bytes out of the staging ring and settles the future.
    ///
    /// **Run when the commit completes, not when the epoch retires.**
    /// A drain waits for the GPU without advancing, and an open epoch's payload never runs — so a
    /// deferral onto the epoch would leave every download in an unadvanced frame unsettled forever.
    cc::vector<pending_download> _pending_downloads;

    /// The counter heaps this list holds while recording, in lease order.
    /// Returned at submit through the epoch that resolved them, or straight back on a drop.
    cc::vector<cc::unique_ptr<metal_counter_heap_lease>> _leased_counter_heaps;

    /// Index into `_leased_counter_heaps` of the one still handing out slots, or -1 before the first timestamp.
    int _active_timestamp_lease = -1;
};
