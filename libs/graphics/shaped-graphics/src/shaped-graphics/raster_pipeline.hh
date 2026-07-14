#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/blend_state.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/depth_stencil_state.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/primitive_topology.hh>
#include <shaped-graphics/rasterization_state.hh>
#include <shaped-graphics/vertex_input.hh>

namespace sg
{
/// The PSO-side state of one color target: the format the pipeline is compiled for, its optional blend
/// equation, and which channels it writes. The command-side counterpart is `color_target` (the actual
/// bound view + clear/discard) — the two must agree on format and order at draw time.
struct color_target_state
{
    pixel_format format = pixel_format::undefined;
    cc::optional<blend_state> blend = {}; ///< unset => the source overwrites the target (no blend)
    color_write_mask write_mask = color_write_mask::all;
};

/// Everything needed to build a raster_pipeline: the shaders, the pipeline_layout they are compiled
/// against, the vertex-input layout, and the fixed-function state (topology, rasterizer, depth-stencil,
/// per-target blend/format). Owns its shaders (a pipeline combines several) so building on a worker
/// thread is safe — parity with raytracing_pipeline_description.
///
/// The color-target formats / sample count live here (not only in the rendering scope) because backends
/// bake them into the PSO (dx12 RTVFormats/DSVFormat, vulkan dynamic-rendering formats). A rendering
/// scope's bound targets must match `color_targets` (count + format) and `depth_stencil_format`.
struct raster_pipeline_description
{
    pipeline_layout_handle layout;

    compiled_shader vertex_shader;                 ///< the vertex stage (required)
    cc::optional<compiled_shader> fragment_shader; ///< the fragment stage; omit for a depth-only pipeline

    /// Optional tessellation stages. Both must be set together (a tessellator needs a hull *and* a domain
    /// stage) or both omitted; when set, `topology` must be `patch_list` and `patch_control_points` > 0.
    cc::optional<compiled_shader> tessellation_control_shader;    ///< hull stage
    cc::optional<compiled_shader> tessellation_evaluation_shader; ///< domain stage

    /// Optional geometry stage — a per-primitive shader that may amplify/emit primitives. Omit for none.
    cc::optional<compiled_shader> geometry_shader;

    vertex_input_layout vertex_input;
    primitive_topology topology = primitive_topology::triangle_list;

    /// Control points per patch — used only when `topology == patch_list` (tessellation). Must be 1..32.
    /// Ignored otherwise.
    int patch_control_points = 0;

    rasterization_state rasterization = {};
    depth_stencil_state depth_stencil = {};

    /// The color targets the pipeline writes, in output-merger order. Empty for a depth-only pipeline.
    cc::fixed_vector<color_target_state, max_color_targets> color_targets;

    /// The depth-stencil target's format, or `undefined` for no depth-stencil attachment.
    pixel_format depth_stencil_format = pixel_format::undefined;

    /// MSAA sample count of the targets (1 = no multisampling).
    int sample_count = 1;

    /// Optional serialized PSO blob for accelerated creation (skips most driver work). Platform-specific
    /// and best-effort: backends may ignore it. Obtain one from a previously-built pipeline via
    /// `cached_pipeline_data()` and persist it across runs.
    cc::pinned_data<cc::byte const> cached_pipeline = {};
};

/// A ready-to-draw raster (graphics) pipeline: vertex + fragment shaders compiled against a
/// pipeline_layout with a fixed-function state set. Bound with cmd.raster.bind_pipeline and drawn inside
/// a rendering scope. Held via raster_pipeline_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 pipeline state + root signature,
/// vulkan VkPipeline + VkPipelineLayout).
class raster_pipeline
{
public:
    virtual ~raster_pipeline();

    /// The backend's serialized PSO blob, for persisting and feeding back via
    /// raster_pipeline_description::cached_pipeline. Empty if the backend doesn't support it.
    [[nodiscard]] virtual cc::pinned_data<cc::byte const> cached_pipeline_data() const = 0;

protected:
    raster_pipeline() = default;
};
} // namespace sg
