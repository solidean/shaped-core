#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/backends/webgpu/webgpu_readback.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/fwd.hh>

/// WebGPU implementation of sg::command_list: one WGPUCommandEncoder, handed out already recording.
///
/// **Passes are opened lazily and closed by whatever needs the encoder.**
/// WebGPU records compute work inside a compute pass and draws inside a render pass, while copies go on the encoder between passes.
/// So a dispatch opens a compute pass if none is open, and a copy closes one; the bound state is replayed when a pass opens, which is what makes the split invisible to sg.
/// A copy recorded inside a rendering scope closes the render pass and reopens it with every load op set to load, since the contents are now real.
///
/// There are no barriers: WebGPU tracks usage itself and transitions nothing sg can see.
class sg::backend::webgpu::webgpu_command_list final : public sg::command_list
{
public:
    webgpu_command_list(webgpu_context& ctx, sg::epoch created_in, wgpu_command_encoder encoder);

    // Auto-drops, with a warning, a list left neither submitted nor dropped.
    ~webgpu_command_list() override;

    /// Ends whichever pass is open, so the encoder can take a command of its own.
    /// A rendering scope stays logically open and reopens at its next draw.
    void end_open_pass();

    /// Finishes the encoder into a command buffer; the list records nothing afterwards.
    [[nodiscard]] wgpu_command_buffer finish();

    /// Every recording path reaches WebGPU through these, so recording from another thread asserts rather than failing inside WebGPU.
    [[nodiscard]] WGPUCommandEncoder encoder() const;
    [[nodiscard]] WGPUComputePassEncoder compute_pass() const;
    [[nodiscard]] WGPURenderPassEncoder render_pass() const;

    webgpu_context& _ctx;
    bool _consumed = false;
    wgpu_command_encoder _encoder;
    wgpu_compute_pass _compute_pass;
    wgpu_render_pass _render_pass;

    /// Whether this list holds spans of the context's upload ring, which it releases at submit or drop.
    bool _holds_upload_ring = false;

    /// Readbacks recorded here, mapped after this list submits and cancelled if it is dropped.
    cc::vector<webgpu_readback> _pending_readbacks;

    /// Constant pages leased to this list, written just before it submits.
    cc::vector<webgpu_constant_page*> _constant_pages;

    /// Timestamp query sets leased by this list, resolved just before it submits.
    cc::vector<webgpu_query_lease*> _query_leases;

    /// Every resource this list names, kept alive until it submits: a transient handle can leave scope before then.
    cc::vector<std::shared_ptr<void const>> _keep_alive;

    /// The buffers and textures this list reads or writes, which submit brings any queued stream into ahead of it.
    cc::vector<void const*> _touched;

    /// Keeps `resource` alive until submit and counts it as touched.
    void touch(sg::raw_buffer_handle const& resource)
    {
        _touched.push_back(resource.get());
        _keep_alive.push_back(resource);
    }
    void touch(sg::raw_texture_handle const& resource)
    {
        _touched.push_back(resource.get());
        _keep_alive.push_back(resource);
    }

    /// Touches everything a bound group names.
    void touch_group(sg::binding_group const& group);

    // What bind_* set up, replayed whenever a pass opens.
    // The pipelines and groups are owning references, since a caller may drop its handle before the replay.
    struct bound_state
    {
        webgpu_pipeline_layout const* layout = nullptr;
        wgpu_compute_pipeline compute_pipeline;
        wgpu_render_pipeline render_pipeline;
        cc::fixed_vector<wgpu_bind_group, sg::max_binding_groups> groups;

        // The inline constants block as the caller last set it, and the placement it was last bound at.
        cc::vector<byte> constants;
        bool constants_dirty = false;
        webgpu_constant_page* constants_page = nullptr;
        u32 constants_offset = 0;
        bool constants_bound = false;

        // Set when anything changed, or a pass opened, since the state was last applied: everything is bound again.
        bool needs_full_apply = true;
    };
    bound_state _compute;
    bound_state _raster;

    // The open rendering scope, kept so a copy in its middle can close and reopen it.
    bool _in_rendering_scope = false;
    struct color_attachment
    {
        wgpu_texture_view view;
        WGPURenderPassColorAttachment attachment = {};
    };
    cc::fixed_vector<color_attachment, sg::max_color_targets> _color_attachments;
    wgpu_texture_view _depth_view;
    WGPURenderPassDepthStencilAttachment _depth_attachment = {};
    bool _has_depth = false;
    tg::vec2i _target_size = tg::vec2i(0, 0);

    // Dynamic raster state, replayed on a reopen.
    struct viewport_state
    {
        float x = 0, y = 0, width = 0, height = 0, min_depth = 0, max_depth = 1;
    };
    viewport_state _viewport;
    tg::aabb2i _scissor;
    u32 _stencil_reference = 0;
    WGPUColor _blend_constants = {0, 0, 0, 0};
    struct vertex_binding
    {
        WGPUBuffer buffer = nullptr;
        u64 offset = 0;
        u64 size = 0;
    };
    cc::fixed_vector<vertex_binding, sg::max_vertex_buffers> _vertex_buffers;
    WGPUBuffer _index_buffer = nullptr;
    WGPUIndexFormat _index_format = WGPUIndexFormat_Undefined;
    u64 _index_offset = 0;

    // The same two facts in sg's own vocabulary, for the alignment rule an indexed draw must satisfy — `_index_format`
    // above is already WebGPU's spelling of the width.
    // See sg::index_buffer_offset_alignment; every backend carries this check.
    sg::index_format _sg_index_format = sg::index_format::uint16;
    isize _index_view_offset_in_bytes = 0;
    u64 _index_size = 0;

    /// Opens the render pass from the stored attachments; `reopen` forces every load op to load.
    void open_render_pass(bool reopen);

    /// Opens a compute pass unless one is open.
    void open_compute_pass();

    /// Binds whatever of `state` is not yet bound on the open pass, the constants included.
    void apply_compute_state();
    void apply_raster_state();

    /// Writes the pending inline constants block into a page, or reuses the placement an unchanged block has.
    void place_constants(bound_state& state);

    /// Resolves the leased query sets and records their readback; runs just before the encoder finishes.
    void finalize_queries();

    /// Returns the leased query sets, resolved or not.
    void release_queries();

    /// Records a readback of a copy already recorded into `readback.staging`, returning its future.
    [[nodiscard]] sg::bytes_future record_readback(webgpu_readback readback,
                                                   cc::pinned_data<byte> destination,
                                                   cc::unique_function<bool(cc::span<byte const>)> deliver);

protected:
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

    // Bodies in webgpu_command_list.compute.cc.
    void compute_bind_pipeline(sg::compute_pipeline const& pipeline) override;
    void compute_bind_group(int group_index, sg::binding_group const& group) override;
    void compute_dispatch(int x, int y, int z) override;
    void compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset) override;
    void compute_declare_array_buffer_access(cc::string_view binding_name,
                                             cc::span<sg::array_buffer_access const> elements) override;
    void compute_declare_array_texture_access(cc::string_view binding_name,
                                              cc::span<sg::array_texture_access const> elements) override;

    // Bodies in webgpu_command_list.raster.cc.
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

    // WebGPU has no ray tracing: support is false, and every recording seam asserts.
    [[nodiscard]] bool raytracing_is_supported() const override { return false; }
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

    // Bodies in webgpu_query.cc.
    [[nodiscard]] bool query_timestamps_supported() const override;
    [[nodiscard]] sg::gpu_timestamp query_record_gpu_timestamp() override;

private:
    /// Stages `data` in the ring and holds the ring until this list submits or drops.
    [[nodiscard]] webgpu_upload_span stage_upload(cc::span<byte const> data, isize staged_size);
};
