#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

#include <memory>

/// Forward declarations and `*_handle` typedefs for shaped-graphics.
/// Include it when a forward decl is all you need.
///
/// It is also sg's single door to `<memory>`, since the smart pointers are sg's own vocabulary: every `*_handle` IS a `std::shared_ptr`, and a command list is a `std::unique_ptr`.
/// Code holding either — inside sg, its backends, or a library above it — includes this header rather than `<memory>`.

namespace sg
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside sg, not leaked globally.
using namespace cc::primitive_defines;

enum class backend_kind;            // which graphics API a context runs on (see types.hh)
enum class thread_model;            // what a context promises about concurrent use (see types.hh)
enum class buffer_usage : u32;      // one buffer usage; a set of them is buffer_usages (see types.hh)
enum class texture_aspect : u32;    // color / depth / stencil planes of a subresource (see resource/subresource.hh)
enum class command_list_slot : int; // a recorder's slot in the per-context pool (see barrier/command_list_slot.hh)

// The vocabulary enums this header goes on to DEFINE rather than only declare, named here so each definition can be written qualified.
enum class index_format : u8;
enum class lifetime_scope;
enum class epoch : u64;
enum class submission_token : u64;

enum class stream_scope : u8;         // how much of a resource one streaming transfer claims (see types.hh)
class context_stream_scope;           // the ctx.stream facade (see transfer/stream.hh)
class stream_upload_handle;           // control handle for one streaming upload (see transfer/stream_handle.hh)
class stream_download_handle;         // …and for one streaming download
struct stream_progress;               // value type — bytes done plus an optional total hint
enum class stream_source_status : u8; // why a source poll returned what it did (see transfer/stream_source.hh)
struct stream_chunk;                  // value type — bytes plus their offset in the destination extent
struct stream_poll;                   // value type — a source poll's status and its chunk
class stream_source;                  // the lazy chunk sequence feeding a streaming upload

class context;
struct adapter_info; // which GPU a context runs on (see context/adapter_info.hh)
class context_persistent_scope;
class context_transient_scope;
class context_upload_scope;
class context_download_scope;
class context_uncached_scope;
class context_cached_scope;
class routine_registry;
class render_routine_base;
template <class Derived>
class render_routine;
template <class Derived>
class routine_guard;
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
class command_list_slot_allocator; // per-recorder slot pool (see barrier/command_list_slot.hh)
struct viewport;                   // value type — one viewport of a rendering_info
struct rendering_info;             // value type — what opens a rendering scope

// Argument value types of the command-list scopes (see command_list/copy.hh and command_list/compute.hh).
struct buffer_bytes_copy;
template <class T>
struct buffer_data_copy;
struct array_buffer_access;
struct array_texture_access;

class raw_buffer;
class raw_texture;
class blas;                         // bottom-level acceleration structure (see raytracing/acceleration_structure.hh)
class tlas;                         // top-level acceleration structure (see raytracing/acceleration_structure.hh)
struct blas_triangles;              // value type — one triangle geometry input to build_blas
struct blas_aabbs;                  // value type — one procedural (AABB) geometry input to build_blas
struct tlas_instance;               // value type — one instance input to build_tlas
enum class accel_build_flag;        // one build-time trade-off; a set of them is accel_build_flags
enum class instance_cull_mode : u8; // per-instance triangle cull selection

} // namespace sg

/// Index-buffer element width — shared by draw index buffers (index_buffer_view) and raytracing BLAS
/// triangle indices (blas_triangles). Defined here (not just forward-declared) as a general vocabulary type.
enum class sg::index_format : sg::u8
{
    uint16, // DX12 R16_UINT / Vk INDEX_TYPE_UINT16
    uint32, // DX12 R32_UINT / Vk INDEX_TYPE_UINT32
};

namespace sg
{
struct texture_description; // value type (see resource/raw_texture.hh) — input to create_raw_texture
struct texture_region;      // value type (see resource/texture_region.hh) — a subresource byte range

// The shape-specific description structs that build a texture_description (see resource/texture_descriptions.hh).
struct texture_1d_description;
struct texture_2d_description;
struct texture_3d_description;
struct texture_cube_description;
struct texture_1d_array_description;
struct texture_2d_array_description;
struct texture_cube_array_description;
struct texture_2d_ms_description;
struct texture_2d_array_ms_description;
struct texture_cube_ms_description;
struct texture_cube_array_ms_description;

enum class pixel_format : u16;     // texel format (see resource/pixel_format.hh)
enum class texture_usage : u32;    // one texture usage; a set of them is texture_usages (see types.hh)
enum class texture_dimension : u8; // 1D / 2D / 3D (see resource/raw_texture.hh)
class bytes_wait_gate;             // deadlock guard on a blocking wait (see bytes_future.hh)
class bytes_future;
template <class T>
class data_future;
class gpu_timestamp; // value type (see query/gpu_timestamp.hh) — result of cmd.query.record_gpu_timestamp
class memory_heap;
struct allocation_info;     // value type (see memory/allocation_info.hh) — no handle typedef
struct memory_requirements; // value type (see memory/memory_heap.hh)

} // namespace sg

/// Lifetime mode of a resource — a hard contract, not a hint.
/// `persistent` lives until its handles are released; `transient` expires when its epoch retires.
/// Using a transient resource past that is a hard error, and the backend may recycle it immediately.
/// Passed to every `create_*`; buffers carry it inside allocation_info.
/// Both modes still get in-flight GPU hazard tracking, which is orthogonal.
enum class sg::lifetime_scope
{
    persistent,
    transient,
};

namespace sg
{

// Backend-neutral access-state vocabulary (see barrier/resource_access.hh / barrier/resource_access_state.hh) — the
// shared, opt-in building blocks a backend uses to track state and emit barriers.
enum class access_flag : u32;         // a set of them is access_flags
enum class pipeline_stage_flag : u32; // a set of them is pipeline_stage_flags
enum class texture_layout : u32;
struct access_barrier;
struct resource_access_state;
struct subresource_box;       // one tile of a covering partition (see barrier/subresource_state.hh)
struct subresource_extent;    // the mip/layer/plane counts of a resource (see resource/subresource.hh)
struct subresource_index;     // one subresource of a resource
struct subresource_range;     // a box of subresources, which one index converts into
struct subresource_partition; // the tiles covering one resource's subresources

// Resource views (see resource/views.hh) — value types, no handle typedefs.
// Only the enums are declared here: the typed view templates are constrained, and `raw_view` is a
// `cc::variant` alias whose arms must be complete where it is named.
// Include resource/views.hh for the views themselves.
enum class view_class;
enum class view_shape;
enum class texture_view_dimension : u8; // shader-facing SRV/UAV dimension (see resource/views.hh)
struct raw_buffer_view;                 // erased buffer-view payload — one arm of raw_view (see resource/views.hh)
struct raw_texture_view;                // erased texture-view payload — one arm of raw_view
struct vacant_view;                     // a vacant array element (no view at all) — one arm of raw_view
struct raw_tlas_view;                   // erased acceleration-structure-view payload — one arm of raw_view
struct tlas_view;                       // the typed acceleration-structure view (see resource/views.hh)
template <class T>
class buffer; // a typed buffer facade (see resource/buffer.hh)

// Render-target / depth-stencil views (see resource/views.hh) — a texture bound as a color / depth-stencil target.
// Not shader-facing; they do not erase to raw_view.
class render_target_view;
class depth_stencil_view;

// Window presentation (see present/swapchain.hh) — a chain of back buffers presented to an OS window.
struct swapchain_description; // value type — input to create_swapchain
class swapchain;
enum class present_mode : u8; // frame pacing (vsync / immediate)

// Rendering-scope targets (see command_list/raster.hh) — a view plus its begin-op (clear / preserve /
// discard). Built via the view's .cleared() / .preserved() / .discarded() members.
enum class target_op : u8;
struct color_target;
struct depth_stencil_target;

// Texture samplers (see binding/sampler.hh) — value types, no handle.
enum class sampler_filter;
enum class sampler_address_mode;
enum class sampler_border_color;
enum class compare_op;
struct sampler;

// Compiled shaders + reflected bindings (see binding/compiled_shader.hh / binding/binding.hh) — value types.
enum class binding_type;
enum class shader_stage;
enum class shader_format;
struct binding;
struct compiler_info;
struct compute_dimensions;
struct compiled_shader;

// Bind path: group schema (binding_group_layout) -> pipeline interface (pipeline_layout) -> pipeline
// (compute_pipeline) -> instance (binding_group). See binding/binding_group_layout.hh / binding/pipeline_layout.hh /
// pipeline/compute_pipeline.hh / binding/binding_group.hh.
class binding_group_layout;
class pipeline_layout;
struct bound_sampler;               // {binding, sampler} — a register-bound static sampler on a pipeline_layout
struct pipeline_layout_description; // {groups, static_samplers} — input to create_pipeline_layout
class compute_pipeline;
struct compute_pipeline_description; // {shader, layout} — input to create_compute_pipeline
class binding_group;
struct bound_view;    // one raw_view (inline) or a vector of them — a named_view's element list
struct named_view;    // {name, bound_view} — input to create_binding_group (one view per array element)
struct named_sampler; // {name, sampler} — static sampler (group layout) / dynamic sampler (group)

// The mutable builder above the immutable group: set descriptors one at a time, snapshot an immutable binding_group out of it.
// See binding/staging_binding_group.hh.
class staging_binding_group;
enum class binding_slot : u32; // defined below — declared here so the definition can be written qualified

// A bindless view over ONE array binding of a staging_binding_group (see binding/bindless_array.hh).
class bindless_array;
class bindless_array_transient_scope;  // array.transient — indices for this epoch only
class bindless_array_persistent_scope; // array.persistent — shared holds whose index outlives the epoch

// One element index into a bindless array, valid ONLY for the epoch it was acquired in.
// A type of its own rather than a u32, so that storing one past its epoch does not compile.
enum class bindless_index : u32;

class bindless_element; // one pinned element, always held through the handle below

namespace impl
{
template <class Key>
class slot_table;           // a bindless_array's key -> element-index map (binding/impl/slot_table.hh)
class bindless_array_state; // what a bindless_array owns, shared so an element may outlive it
} // namespace impl

// Raster (graphics) pipeline + its fixed-function state vocabulary (see pipeline/raster_pipeline.hh and the
// pipeline/primitive_topology.hh / pipeline/rasterization_state.hh / pipeline/blend_state.hh / pipeline/depth_stencil_state.hh /
// pipeline/vertex_input.hh state headers). All value types unless noted.
enum class primitive_topology;
enum class primitive_topology_type;
enum class fill_mode;
enum class cull_mode;
enum class front_face;
struct rasterization_state;
enum class blend_factor;
enum class blend_op;
enum class color_channel : u8; // a set of them is color_write_mask
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

// Draw recording (see command_list/raster.hh) — vertex/index buffer views + draw parameters.
// (index_format is defined above — shared with raytracing.)
struct vertex_buffer_view;
struct index_buffer_view;
struct draw_config;
struct draw_indexed_config;

// Ray-tracing pipeline + shader table (see raytracing/raytracing_pipeline.hh / raytracing/raytracing_shader_table.hh). A
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
/// budget). Indexes into pipeline_layout_description::groups and cmd.compute.bind_group's `group_index`.
inline constexpr int max_binding_groups = 4;

// The next two caps are real GPU pipeline limits rather than arbitrary array sizes — an output-merger
// has a fixed handful of color slots, an input assembler a fixed handful of vertex-buffer slots.
// Each is set to the portable floor across tier-1/2 backends, so a layout stays portable.
// Each also bounds a fixed_vector, so an overflow is a hard error rather than a silent heap allocation.

/// Hard cap on simultaneous color render targets in a rendering scope / raster pipeline.
/// 8 is the DX12 (`D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT`) and WebGPU (`maxColorAttachments`) limit.
/// Vulkan guarantees at least 4, and is 8 on essentially all desktop adapters.
inline constexpr int max_color_targets = 8;

/// Hard cap on vertex-buffer input slots bound for a draw.
/// 8 is WebGPU's `maxVertexBuffers`; DX12 allows 32 (`D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT`) and Vulkan at least 16.
inline constexpr int max_vertex_buffers = 8;

} // namespace sg

/// A binding's identity inside a staging_binding_group, resolved once from its name.
/// Opaque: it is an index into the group's internal slot table, not a descriptor position — that indirection
/// is what carries a binding's heap, its first descriptor and its element count, and it is where every set is bounds-checked.
/// Only meaningful for the group it came from, and `invalid` is what an unknown name resolves to.
enum class sg::binding_slot : sg::u32
{
    invalid = ~u32(0),
};

/// Frame-level GPU lifetime token, and a direct-queue timeline value.
/// A monotonic counter where reaching value N on the queue's epoch fence means all GPU work of epoch N has finished.
/// See libs/graphics/shaped-graphics/docs/concepts/epochs.md.
enum class sg::epoch : sg::u64
{
    invalid = 0,   ///< null sentinel — "not meaningfully set"
    first = 10000, ///< first live value; deliberately high so an accidental zero-init is obviously wrong
};

/// Finer-grained per-command-list completion token on the direct queue's submission fence.
/// Monotonic, so "is this one list done?" is a single compare against the fence's completed value.
enum class sg::submission_token : sg::u64
{
    invalid = 0,             ///< null sentinel
    first = 30000,           ///< first live value (see epoch::first for the high-value rationale)
    not_submitted = u64(-1), ///< sentinel that always compares "not yet complete"
};

namespace sg
{

// The exception hierarchy (see exceptions.hh) — what the throwing create façades and submit/advance raise.
class exception;
class device_lost_exception;
class allocation_exception;
class pipeline_creation_exception;
class swapchain_creation_exception;
class binding_group_exception;

/// A `*_handle` is a std::shared_ptr to a shared-lifetime sg type.
/// context, buffer and memory_heap get handles; command_list does not, being a single-use temporary held by std::unique_ptr and passed by reference.
/// cc::shared_ptr exists, but its Traits protocol is still provisional and sg's resources need a base-keyed Traits it does not have, so the switch is deliberately deferred.
/// See libs/graphics/shaped-graphics/docs/coding-guidelines.md.
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
// Mutable, unlike every other resource handle here: a staging group exists to be set, and snapshot() caches on it.
using staging_binding_group_handle = std::shared_ptr<staging_binding_group>;
using swapchain_handle = std::shared_ptr<swapchain>; // mutable: a swapchain is a per-frame driver (acquire/present)
// A pinned bindless element: the refcount IS the pin, so the slot frees when the last copy dies.
using bindless_element_handle = std::shared_ptr<bindless_element const>;

// Async result handles for cached shader compilation / async pipeline build (see context_cached_scope,
// pipeline_cache, and the shaped-shader-compiler-dxc shader_cache). These are the producing cc::async<T>
// rather than the read-only cc::async<T const>, so const arrives at the read side: an async's try_value()
// yields a non-owning `T const*` into the node — hold the async handle below to keep it alive while you read
// (a shareable projection back to a *_handle is a deferred follow-up).
using async_compiled_shader = cc::shared_async<compiled_shader>;          // try_value() -> compiled_shader const*
using async_compute_pipeline = cc::shared_async<compute_pipeline_handle>; // blocking_get -> compute_pipeline_handle
using async_raster_pipeline = cc::shared_async<raster_pipeline_handle>;   // blocking_get -> raster_pipeline_handle
using async_raytracing_pipeline
    = cc::shared_async<raytracing_pipeline_handle>; // blocking_get -> raytracing_pipeline_handle

/// The domain every recording site in shaped-graphics is attributed to.
/// Each backend shadows it with one of its own, so a dx12 message is never mistaken for a portable one.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sg
