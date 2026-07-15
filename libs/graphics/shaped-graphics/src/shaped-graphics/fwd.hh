#pragma once

#include <clean-core/fwd.hh>

#include <memory>

/// Forward declarations and `*_handle` typedefs for shaped-graphics. Include when a forward decl is
/// all you need.

namespace sg
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside sg, not leaked globally.
using namespace cc::primitive_defines;

class context;
class context_persistent_scope;
class context_transient_scope;
class context_upload_scope;
class context_download_scope;
class context_uncached_scope;
class context_cached_scope;
class pipeline_cache;
class command_list;
class command_list_upload_scope;
class command_list_download_scope;
class command_list_copy_scope;
class command_list_compute_scope;
class command_list_raytracing_scope;
class command_list_query_scope;
class command_list_raster_scope;
class command_list_raster_manual_scope;
class rendering_scope;
class raw_buffer;
class raw_texture;
class blas;                         // bottom-level acceleration structure (see acceleration_structure.hh)
class tlas;                         // top-level acceleration structure (see acceleration_structure.hh)
struct blas_triangles;              // value type — one triangle geometry input to build_blas
struct blas_aabbs;                  // value type — one procedural (AABB) geometry input to build_blas
struct tlas_instance;               // value type — one instance input to build_tlas
enum class accel_build_flags : u32; // build-time trade-offs (see acceleration_structure.hh)
enum class instance_cull_mode : u8; // per-instance triangle cull selection

/// Index-buffer element width — shared by draw index buffers (index_buffer_view) and raytracing BLAS
/// triangle indices (blas_triangles). Defined here (not just forward-declared) as a general vocabulary type.
enum class index_format : u8
{
    uint16, // DX12 R16_UINT / Vk INDEX_TYPE_UINT16
    uint32, // DX12 R32_UINT / Vk INDEX_TYPE_UINT32
};
struct texture_description;        // value type (see raw_texture.hh) — input to create_raw_texture
enum class pixel_format : u16;     // texel format (see pixel_format.hh)
enum class texture_usage : u32;    // texture usage flags (see types.hh)
enum class texture_dimension : u8; // 1D / 2D / 3D (see raw_texture.hh)
class bytes_waiter;
class bytes_future;
template <class T>
class data_future;
class gpu_timestamp; // value type (see gpu_timestamp.hh) — result of cmd.query.record_gpu_timestamp
class memory_heap;
struct allocation_info;     // value type (see allocation_info.hh) — no handle typedef
struct memory_requirements; // value type (see memory_heap.hh)

/// Lifetime mode of a resource — a hard contract, not a hint. `persistent` lives until its handles are
/// released; `transient` expires when its epoch retires (using it beyond that is a hard error, and the
/// backend may recycle it immediately). Passed to every `create_*` (buffers carry it inside
/// allocation_info). Both modes still get in-flight GPU hazard tracking, which is orthogonal.
enum class lifetime_scope
{
    persistent,
    transient,
};

// Backend-neutral access-state vocabulary (see resource_access.hh / resource_access_state.hh) — the
// shared, opt-in building blocks a backend uses to track state and emit barriers.
enum class access_flags : u32;
enum class pipeline_stage_flags : u32;
enum class texture_layout : u32;
struct access_barrier;
struct resource_access_state;

// Resource views (see views.hh) — value types, no handle typedefs. The typed view templates
// (uniform_buffer_view/readonly_buffer_view/readwrite_buffer_view) are constrained, and `raw_view` is a
// `std::variant` alias (not forward-declarable), so only the enums are declared here; include views.hh for
// the views themselves.
enum class view_class;
enum class view_shape;
enum class texture_view_dimension : u8; // shader-facing SRV/UAV dimension (see views.hh)
struct raw_buffer_view;                 // erased buffer-view payload — one arm of raw_view (see views.hh)
struct raw_texture_view;                // erased texture-view payload — one arm of raw_view
struct raw_tlas_view;                   // erased acceleration-structure-view payload — one arm of raw_view

// Render-target / depth-stencil views (see views.hh) — a texture bound as a color / depth-stencil target.
// Not shader-facing; they do not erase to raw_view.
class render_target_view;
class depth_stencil_view;

// Window presentation (see swapchain.hh) — a chain of back buffers presented to an OS window.
struct swapchain_description; // value type — input to create_swapchain
class swapchain;
enum class present_mode : u8; // frame pacing (vsync / immediate)

// Rendering-scope targets (see command_list.raster.hh) — a view plus its begin-op (clear / preserve /
// discard). Built via the view's .cleared() / .preserved() / .discarded() members.
enum class target_op : u8;
struct color_target;
struct depth_stencil_target;

// Texture samplers (see sampler.hh) — value types, no handle.
enum class sampler_filter;
enum class sampler_address_mode;
enum class sampler_border_color;
enum class compare_op;
struct sampler;

// Compiled shaders + reflected bindings (see compiled_shader.hh / binding.hh) — value types.
enum class binding_type;
enum class shader_stage;
enum class shader_format;
struct binding;
struct compiler_info;
struct compute_dimensions;
struct compiled_shader;

// Bind path: group schema (binding_group_layout) -> pipeline interface (pipeline_layout) -> pipeline
// (compute_pipeline) -> instance (binding_group). See binding_group_layout.hh / pipeline_layout.hh /
// compute_pipeline.hh / binding_group.hh.
class binding_group_layout;
class pipeline_layout;
struct bound_sampler;               // {binding, sampler} — a register-bound static sampler on a pipeline_layout
struct pipeline_layout_description; // {groups, static_samplers} — input to create_pipeline_layout
class compute_pipeline;
struct compute_pipeline_description; // {shader, layout} — input to create_compute_pipeline
class binding_group;
struct named_view;    // {name, raw_view} — input to create_binding_group
struct named_sampler; // {name, sampler} — static sampler (group layout) / dynamic sampler (group)

// Raster (graphics) pipeline + its fixed-function state vocabulary (see raster_pipeline.hh and the
// primitive_topology.hh / rasterization_state.hh / blend_state.hh / depth_stencil_state.hh /
// vertex_input.hh state headers). All value types unless noted.
enum class primitive_topology;
enum class primitive_topology_type;
enum class fill_mode;
enum class cull_mode;
enum class front_face;
struct rasterization_state;
enum class blend_factor;
enum class blend_op;
enum class color_write_mask : u8;
struct blend_component;
struct blend_state;
enum class stencil_op;
struct stencil_face;
struct depth_stencil_state;
enum class vertex_attribute_format;
struct vertex_attribute;
struct vertex_input_slot;
struct vertex_input_layout;
struct vertex_type_layout;
class raster_pipeline;
struct color_target_state;          // {format, blend, write_mask} — one color target's PSO state
struct raster_pipeline_description; // {layout, shaders, vertex_input, state, ...} — input to create_raster_pipeline

// Draw recording (see command_list.raster.hh) — vertex/index buffer views + draw parameters.
// (index_format is defined above — shared with raytracing.)
struct vertex_buffer_view;
struct index_buffer_view;
struct draw_config;
struct draw_indexed_config;

// Ray-tracing pipeline + shader table (see raytracing_pipeline.hh / raytracing_shader_table.hh). A
// DXR state object plus a table of shader identifiers; dispatched via cmd.raytracing.dispatch_rays.
class raytracing_pipeline;
struct raytracing_pipeline_description; // {layout, raygen/miss/hit/callable shaders, limits} — input to create
struct hit_shader;                      // {closest_hit, any_hit, intersection} — one hit group's shaders
class raytracing_shader_table;
struct raytracing_shader_table_description; // {pipeline, raygen/miss/hit/callable handles} — input to create

// Two-phase model: a *_shader_handle registers a shader in a raytracing_pipeline; a *_index is a slot in a
// raytracing_shader_table (what HLSL TraceRay addresses at dispatch). Strongly-typed integer newtypes.
enum class raygen_shader_handle : u32;
enum class miss_shader_handle : u32;
enum class hit_shader_handle : u32;
enum class callable_shader_handle : u32;
enum class raygen_index : u32;
enum class miss_index : u32;
enum class hit_index : u32;
enum class callable_index : u32;

/// Hard cap on the number of group slots a pipeline_layout can hold (dx12 root-parameter / vulkan set
/// budget). Indexes into pipeline_layout_description::groups and cmd.compute.bind_group's `set`.
inline constexpr int max_binding_groups = 4;

// The next two caps are small because they are real GPU pipeline limits, not arbitrary array sizes: a
// GPU's output-merger has a fixed handful of color-output slots and its input assembler a fixed handful
// of vertex-buffer slots. They bound the (fixed_vector) containers holding those bindings, so an overflow
// is a hard error rather than a silent heap allocation. Chosen as the portable value across tier-1/2
// backends (the minimum any of them guarantees), so a layout stays portable.

/// Hard cap on simultaneous color render targets in a rendering scope / raster pipeline. 8 is the DX12
/// (`D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT`) and WebGPU (`maxColorAttachments`) limit; Vulkan guarantees
/// at least 4 and is 8 on essentially all desktop adapters.
inline constexpr int max_color_targets = 8;

/// Hard cap on vertex-buffer input slots bound for a draw. 8 is WebGPU's `maxVertexBuffers` — the portable
/// floor; DX12 allows 32 (`D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT`) and Vulkan at least 16, but capping
/// at the common denominator keeps a vertex layout portable across every backend.
inline constexpr int max_vertex_buffers = 8;

/// Frame-level GPU lifetime token and direct-queue timeline value: a monotonic counter where
/// reaching value N on the queue's epoch fence means all GPU work of epoch N has finished. See
/// libs/graphics/shaped-graphics/docs/concepts/epochs.md.
enum class epoch : u64
{
    invalid = 0,   ///< null sentinel — "not meaningfully set"
    first = 10000, ///< first live value; deliberately high so an accidental zero-init is obviously wrong
};

/// Finer-grained per-command-list completion token on the direct queue's submission fence.
/// Monotonic, so "is this one list done?" is a single compare against the fence's completed value.
enum class submission_token : u64
{
    invalid = 0,             ///< null sentinel
    first = 30000,           ///< first live value (see epoch::first for the high-value rationale)
    not_submitted = u64(-1), ///< sentinel that always compares "not yet complete"
};

/// A `*_handle` is a std::shared_ptr to a shared-lifetime sg type. context, buffer, and memory_heap get
/// handles; command_list does not — it's a single-use temporary held by std::unique_ptr, passed by
/// reference. std::shared_ptr is a placeholder for a future cc::shared_ptr.
using context_handle = std::shared_ptr<context>;
using raw_buffer_handle = std::shared_ptr<raw_buffer const>; // shared-immutable: a view/handle can't reshape the buffer
using raw_texture_handle = std::shared_ptr<raw_texture const>; // shared-immutable: shape is fixed at creation
using blas_handle = std::shared_ptr<blas const>;               // shared-immutable: an opaque, driver-built structure
using tlas_handle = std::shared_ptr<tlas const>; // shared-immutable: indexes a set of instances of blas_handle
using memory_heap_handle = std::shared_ptr<memory_heap const>;         // immutable resource — it tracks no allocations
using compiled_shader_handle = std::shared_ptr<compiled_shader const>; // immutable compiled shader + reflection
using binding_group_layout_handle = std::shared_ptr<binding_group_layout const>; // immutable per-group schema
using pipeline_layout_handle = std::shared_ptr<pipeline_layout const>;           // immutable ordered group layouts
using compute_pipeline_handle = std::shared_ptr<compute_pipeline const>;
using raster_pipeline_handle = std::shared_ptr<raster_pipeline const>; // immutable graphics PSO + root signature
using raytracing_pipeline_handle = std::shared_ptr<raytracing_pipeline const>; // immutable DXR state object + shader ids
using raytracing_shader_table_handle = std::shared_ptr<raytracing_shader_table const>; // immutable table over a pipeline
using binding_group_handle = std::shared_ptr<binding_group const>; // immutable once bound (recreate to rebind)
using swapchain_handle = std::shared_ptr<swapchain>; // mutable: a swapchain is a per-frame driver (acquire/present)

// Async result handles for cached shader compilation / async pipeline build (see context_cached_scope,
// pipeline_cache, and the shaped-shader-compiler-dxc shader_cache). cc::async<T> cannot hold a const T
// (its internal cc::optional<T> forbids it), so const arrives at the read side: an async's try_value()
// yields the matching const *_handle above.
using async_compiled_shader = std::shared_ptr<cc::async<compiled_shader>>; // try_value() -> compiled_shader_handle
using async_compute_pipeline
    = std::shared_ptr<cc::async<compute_pipeline_handle>>; // blocking_get -> compute_pipeline_handle
using async_raytracing_pipeline
    = std::shared_ptr<cc::async<raytracing_pipeline_handle>>; // blocking_get -> raytracing_pipeline_handle
} // namespace sg
