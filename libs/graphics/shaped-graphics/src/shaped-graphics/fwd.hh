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
class context_cached_scope;
class pipeline_cache;
class command_list;
class command_list_upload_scope;
class command_list_download_scope;
class command_list_copy_scope;
class command_list_compute_scope;
class raw_buffer;
class raw_texture;
struct texture_description;        // value type (see raw_texture.hh) — input to create_raw_texture
enum class pixel_format : u16;     // texel format (see pixel_format.hh)
enum class texture_usage : u32;    // texture usage flags (see types.hh)
enum class texture_dimension : u8; // 1D / 2D / 3D (see raw_texture.hh)
class bytes_waiter;
class bytes_future;
template <class T>
class data_future;
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
// (uniform_view/readonly_view/readwrite_view) are constrained, so only the enums and raw_view are
// forward-declared here; include views.hh for the views themselves.
enum class view_class;
enum class view_shape;
enum class texture_view_dimension : u8; // shader-facing SRV/UAV dimension (see views.hh)
struct raw_view;

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

// Bind path: schema (binding_layout) -> pipeline (compute_pipeline) -> instance (binding_group). See
// binding_layout.hh / compute_pipeline.hh / binding_group.hh.
class binding_layout;
class compute_pipeline;
struct compute_pipeline_description; // {shader, layout} — input to create_compute_pipeline
class binding_group;
struct named_view;    // {name, raw_view} — input to create_binding_group
struct named_sampler; // {name, sampler} — static sampler (layout) / dynamic sampler (group)

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
using raw_texture_handle = std::shared_ptr<raw_texture const>;         // shared-immutable: shape is fixed at creation
using memory_heap_handle = std::shared_ptr<memory_heap const>;         // immutable resource — it tracks no allocations
using compiled_shader_handle = std::shared_ptr<compiled_shader const>; // immutable compiled shader + reflection
using binding_layout_handle = std::shared_ptr<binding_layout const>;   // immutable schema
using compute_pipeline_handle = std::shared_ptr<compute_pipeline const>;
using binding_group_handle = std::shared_ptr<binding_group const>; // immutable once bound (recreate to rebind)

// Async result handles for cached shader compilation / async pipeline build (see context_cached_scope,
// pipeline_cache, and the shaped-shader-compiler-dxc shader_cache). cc::async<T> cannot hold a const T
// (its internal cc::optional<T> forbids it), so const arrives at the read side: an async's try_value()
// yields the matching const *_handle above.
using async_compiled_shader = std::shared_ptr<cc::async<compiled_shader>>; // try_value() -> compiled_shader_handle
using async_compute_pipeline
    = std::shared_ptr<cc::async<compute_pipeline_handle>>; // blocking_get -> compute_pipeline_handle
using async_binding_layout
    = std::shared_ptr<cc::async<binding_layout_handle>>; // defined for future/graphics use — layout acquire is SYNC today
} // namespace sg
