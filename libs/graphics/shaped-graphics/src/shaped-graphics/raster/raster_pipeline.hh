#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/query/driver_statistic.hh>
#include <shaped-graphics/raster/blend_state.hh>
#include <shaped-graphics/raster/depth_stencil_state.hh>
#include <shaped-graphics/raster/primitive_topology.hh>
#include <shaped-graphics/raster/raster_target_formats.hh>
#include <shaped-graphics/raster/rasterization_state.hh>
#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-graphics/resource/pixel_format.hh>

/// The PSO-side state of one color target: the format the pipeline is compiled for, its optional blend equation, and which channels it writes.
/// The command-side counterpart is `color_target`, the actual bound view plus clear/discard — the two must agree on format and order at draw time.
struct sg::color_target_state
{
    pixel_format format = pixel_format::undefined;
    cc::optional<blend_state> blend = {}; ///< unset => the source overwrites the target (no blend)
    color_write_mask write_mask = color_write_mask_all;
};

/// A mesh stage drawn where the device has none: a compute pass writes each workgroup's output into fixed slots of a
/// buffer, and the pipeline's vertex stage draws them back.
/// SGL writes both halves of one mesh entry point; libs/graphics/shaped-graphics/docs/concepts/mesh-polyfill.md is the
/// protocol, and these are its numbers.
struct sg::mesh_polyfill_description
{
    /// The compute half, at the mesh stage's workgroup size, which writes `sg_mesh_output`.
    compiled_shader compute_shader;
    /// 3 for triangles, 2 for lines.
    int corners = 3;
    /// The mesh stage's `triangles` or `lines` bound.
    int primitives_per_group = 0;
    /// The 32-bit words one workgroup's slots take.
    int words_per_group = 0;
    /// 0 for a mesh stage.
    /// For a geometry stage drawn this way, the corners of each primitive a draw assembles from the bound buffers, 1, 2
    /// or 3: a primitive's output then takes a workgroup's slots, and the pipeline draws with `draw` (SGL's CHK-525).
    int input_corners = 0;

    /// A task stage before the mesh stage, drawn the same way: its compute form, which writes each task workgroup's
    /// record of `sg_task_output`, and the scan that numbers what they launched; `compute_shader` then reads them.
    /// The draw's mesh workgroups are then the GPU's to count, so the draw is indirect.
    cc::optional<compiled_shader> task_shader;
    compiled_shader task_scan_shader;
    /// The 32-bit words of one task workgroup's record.
    int task_record_words = 0;
    /// The mesh workgroups one task workgroup launches at most, which a draw holds slots for; 0 where the task stage
    /// states none, and `task_budget` bounds the draw instead.
    int task_meshes = 0;
    /// The mesh workgroups a draw of a task stage that states no bound launches at most; the excess is dropped.
    /// A host may set it for a pipeline; at most `max_dispatch_groups_per_dimension`.
    int task_budget = 4096;
};

/// What a polyfilled raster pipeline draws with, which the context attaches once both halves are built.
struct sg::impl::mesh_polyfill_state
{
    compute_pipeline_handle compute;
    /// The compute half's workgroup size, which the grid of threads it reads is counted in.
    compute_dimensions workgroup;
    int corners = 3;
    int primitives_per_group = 0;
    int words_per_group = 0;
    /// `mesh_polyfill_description::input_corners`: 0 for a mesh stage, the assembled primitive's for a geometry stage.
    int input_corners = 0;
    /// A polyfilled task stage's compute form and scan, both null without one; `compute` is then dispatched indirectly.
    compute_pipeline_handle task;
    compute_pipeline_handle task_scan;
    compute_dimensions task_workgroup;
    int task_record_words = 0;
    int task_meshes = 0;
    int task_budget = 0;

    /// The mesh workgroups a draw of `tasks` task workgroups holds slots for.
    [[nodiscard]] isize task_capacity(isize tasks) const
    {
        auto const wanted = task_meshes > 0 ? tasks * isize(task_meshes) : isize(task_budget);
        return wanted < isize(max_dispatch_groups_per_dimension) ? wanted : isize(max_dispatch_groups_per_dimension);
    }
};

/// One draw of a polyfilled geometry stage, as its compute half reads it from `sg_geometry_draw`.
struct sg::impl::geometry_polyfill_draw
{
    /// The first vertex, or the first index of an indexed draw.
    u32 first = 0;
    /// The primitives of one instance: the draw's vertices or indices over the assembled primitive's corners.
    u32 primitives = 0;
    u32 instances = 0;
    u32 first_instance = 0;
    bool is_indexed = false;
    /// What an indexed draw adds to each index before the fetch.
    i32 vertex_offset = 0;
};

/// Everything needed to build a raster_pipeline: the shaders, the pipeline_layout they are compiled against, the vertex-input layout, and the fixed-function state.
/// It owns its shaders, since a pipeline combines several and building on a worker thread must be safe — parity with raytracing_pipeline_description.
///
/// The color-target formats and sample count live here, not only in the rendering scope, because backends bake them into the PSO.
/// That is dx12 RTVFormats / DSVFormat, and vulkan's dynamic-rendering formats.
/// A rendering scope's bound targets must match `color_targets` in count and format, and `depth_stencil_format`.
struct sg::raster_pipeline_description
{
    pipeline_layout_handle layout;

    compiled_shader vertex_shader;                 ///< the vertex stage, required unless `mesh_shader` is set
    cc::optional<compiled_shader> fragment_shader; ///< the fragment stage; omit for a depth-only pipeline

    /// Optional tessellation stages, both set together or both omitted — a tessellator needs a hull *and* a domain stage.
    /// When set, `topology` must be `patch_list` and `patch_control_points` > 0.
    cc::optional<compiled_shader> tessellation_control_shader;    ///< hull stage
    cc::optional<compiled_shader> tessellation_evaluation_shader; ///< domain stage

    /// Optional geometry stage — a per-primitive shader that may amplify or emit primitives.
    /// Omit it for none.
    cc::optional<compiled_shader> geometry_shader;

    /// Optional mesh stage, which replaces the vertex stage and everything before the rasterizer: its workgroups write the vertices and primitives.
    /// When set, `vertex_shader` must be empty, `vertex_input` must hold no buffer, and no tessellation or geometry stage may be set.
    /// The pipeline draws with `draw_mesh_tasks`, never `draw`; it needs sg::feature::mesh_shader.
    cc::optional<compiled_shader> mesh_shader;
    /// Optional task stage before `mesh_shader`, whose workgroups each launch mesh workgroups and hand them one payload.
    cc::optional<compiled_shader> task_shader;
    /// A mesh stage as its polyfill, in place of `mesh_shader`: `vertex_shader` is its vertex half, and the pipeline still
    /// draws with `draw_mesh_tasks`, where the device lacks sg::feature::mesh_shader or not.
    /// It takes no vertex input, no task, tessellation or geometry stage, and `topology` must match its `corners`.
    cc::optional<mesh_polyfill_description> mesh_polyfill;

    vertex_input_layout vertex_input;
    primitive_topology topology = primitive_topology::triangle_list;

    /// Control points per patch, used only when `topology == patch_list` and ignored otherwise.
    /// Must be 1..32.
    int patch_control_points = 0;

    rasterization_state rasterization = {};
    depth_stencil_state depth_stencil = {};

    /// The color targets the pipeline writes, in output-merger order.
    /// Empty for a depth-only pipeline.
    cc::fixed_vector<color_target_state, max_color_targets> color_targets;

    /// The depth-stencil target's format, or `undefined` for no depth-stencil attachment.
    pixel_format depth_stencil_format = pixel_format::undefined;

    /// MSAA sample count of the targets (1 = no multisampling).
    int sample_count = 1;

    /// The views one draw renders, view `i` into layer `i` of every target's view, which SGL's `@view_index` reads.
    /// More than 1 needs sg::feature::view_instancing, and a rendering scope of the same `view_count` to draw in.
    /// dx12 takes at most 4 views, vulkan its device's `maxMultiviewViewCount`.
    int view_count = 1;

    /// The name of the target set these color targets are, as a generated SGL render target states it; empty for none.
    /// A pipeline that names one refuses to bind into a rendering that names another, even where the formats agree.
    cc::string target_set;

    /// Optional serialized PSO blob for accelerated creation, skipping most driver work.
    /// Platform-specific and best-effort: a backend may ignore it.
    /// Obtain one from a previously-built pipeline via `cached_pipeline_data()` and persist it across runs.
    cc::pinned_data<byte const> cached_pipeline = {};
};

/// A ready-to-draw raster (graphics) pipeline: vertex + fragment shaders compiled against a pipeline_layout, with a fixed-function state set.
/// Bound with cmd.raster.bind_pipeline and drawn inside a rendering scope.
/// Held via raster_pipeline_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 pipeline state + root signature,
/// vulkan VkPipeline + VkPipelineLayout).
class sg::raster_pipeline : public std::enable_shared_from_this<raster_pipeline>
{
public:
    virtual ~raster_pipeline();

    /// Destroys the backend objects now, leaving this an inert husk.
    ///
    /// The context calls this at shutdown for everything its pipeline cache still holds, because the cache dropping
    /// its own reference is NOT what frees them: a scheduled build node keeps the handle alive on a pool worker, and
    /// that node's later drop would run this destructor against a context that no longer exists.
    /// See libs/graphics/shaped-graphics/docs/concepts/caches.md, "Shutdown releases the objects, not just the entries".
    ///
    /// Idempotent, and the destructor afterwards does nothing.
    /// The default is empty, for a backend whose objects are reference-counted and safe to drop late.
    virtual void release_backend_objects() {}

    /// The backend's serialized PSO blob, for persisting and feeding back via raster_pipeline_description::cached_pipeline.
    /// Empty if the backend does not support it.
    [[nodiscard]] virtual cc::pinned_data<byte const> cached_pipeline_data() const = 0;

    /// What the driver reports of the code it compiled for this pipeline: registers, spills, instructions.
    /// Empty where the backend has no such report, or its context was not asked to capture one
    /// (`vulkan_config::capture_driver_statistics`).
    [[nodiscard]] virtual cc::vector<driver_statistic> driver_statistics() const { return {}; }

    /// The forms the driver kept of this pipeline's code, its machine code among them where it gives that.
    /// Empty where the backend has none, or its context was not asked to keep them
    /// (`vulkan_config::capture_driver_representations`).
    [[nodiscard]] virtual cc::vector<driver_representation> driver_representations() const { return {}; }

    /// Whether creation actually consumed the `cached_pipeline` blob it was handed.
    ///
    /// False when none was supplied, and false when one was supplied and the backend rejected it.
    /// A rejection is the exact signal that a persisted blob has gone stale — a driver update, a different adapter —
    /// so a cache replaces its entry on it rather than trying to predict staleness through the key.
    [[nodiscard]] bool used_cached_pipeline() const { return _used_cached_pipeline; }

    /// The description's `target_set`: the name of the targets this pipeline writes, or empty.
    [[nodiscard]] cc::string_view target_set() const { return _target_set; }

    /// The target formats and sample count this pipeline was built for, which binding it checks a rendering against.
    /// Empty only for a pipeline a backend built outside the context's creation paths.
    [[nodiscard]] cc::optional<raster_target_formats> const& target_formats() const { return _target_formats; }

    /// Whether a mesh stage draws for this pipeline, which `draw_mesh_tasks` needs and every other draw refuses.
    /// A polyfilled mesh stage is one.
    [[nodiscard]] bool is_mesh_shaded() const { return _is_mesh_shaded; }

    /// What a polyfilled mesh stage's draw runs, or null where the mesh stage is native or there is none.
    [[nodiscard]] impl::mesh_polyfill_state const* mesh_polyfill() const
    {
        return _mesh_polyfill.has_value() ? &_mesh_polyfill.value() : nullptr;
    }

    /// What this pipeline's code does to each binding, resolved against its layout, which dispatch follows for barriers.
    /// Unknown when any stage's shader carried none, or named a binding the layout does not hold.
    [[nodiscard]] impl::pipeline_footprint const& footprint() const { return _footprint; }

protected:
    raster_pipeline() = default;

    /// Set by the backend during creation; see used_cached_pipeline().
    bool _used_cached_pipeline = false;

private:
    friend void impl::set_targets(raster_pipeline const& pipeline,
                                  cc::string_view target_set,
                                  raster_target_formats const& formats,
                                  bool is_mesh_shaded);

    friend void impl::set_footprint(raster_pipeline const& pipeline, impl::pipeline_footprint footprint);
    friend void impl::set_mesh_polyfill(raster_pipeline const& pipeline, impl::mesh_polyfill_state state);

    cc::string _target_set;
    cc::optional<raster_target_formats> _target_formats;
    impl::pipeline_footprint _footprint;
    bool _is_mesh_shaded = false;
    cc::optional<impl::mesh_polyfill_state> _mesh_polyfill;
};
