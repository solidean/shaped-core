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
class command_list;
class command_list_upload_scope;
class command_list_download_scope;
class command_list_copy_scope;
class command_list_compute_scope;
class raw_buffer;
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
enum class command_list_slot : int;
class command_list_slot_allocator;

// Resource views (see views.hh) — value types, no handle typedefs. The typed view templates
// (uniform_view/readonly_view/readwrite_view) are constrained, so only the enums and raw_view are
// forward-declared here; include views.hh for the views themselves.
enum class view_class;
enum class view_shape;
struct raw_view;

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
struct named_view; // {name, raw_view} — input to create_binding_group

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
using memory_heap_handle = std::shared_ptr<memory_heap const>;         // immutable resource — it tracks no allocations
using compiled_shader_handle = std::shared_ptr<compiled_shader const>; // immutable compiled shader + reflection
using binding_layout_handle = std::shared_ptr<binding_layout const>;   // immutable schema
using compute_pipeline_handle = std::shared_ptr<compute_pipeline const>;
using binding_group_handle = std::shared_ptr<binding_group const>; // immutable once bound (recreate to rebind)
} // namespace sg
