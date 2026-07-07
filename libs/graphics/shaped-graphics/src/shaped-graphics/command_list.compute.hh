#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backend/resource_access.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Per-element access for a *buffer* array bound to a shader — the payload of `declare_array_buffer_access`.
/// Buffers have no layout, so only the accessed element, stage(s), and access are named.
struct array_buffer_access
{
    int index = 0;                                            ///< element index within the bound array
    pipeline_stage_flags stages = pipeline_stage_flags::none; ///< stage(s) the shader accesses it in
    access_flags access = access_flags::none;                 ///< how the shader accesses this element
};

/// Per-element access for a *texture* array bound to a shader — the payload of `declare_array_texture_access`.
/// Adds the layout the element must be in (and, later, a subresource range).
struct array_texture_access
{
    int index = 0;                                            ///< element index within the bound array
    pipeline_stage_flags stages = pipeline_stage_flags::none; ///< stage(s) the shader accesses it in
    access_flags access = access_flags::none;                 ///< how the shader accesses this element
    texture_layout layout = texture_layout::general;          ///< the layout the element must be in
    // A subresource range (which mips / array slices / aspects) is a future addition — see subresource.hh.
};

/// Compute recording facade for a command list, reached as `cmd.compute`: bind a pipeline + resource
/// groups, then dispatch.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_compute_scope
{
public:
    /// Makes `pipeline` the active compute pipeline for subsequent bind_group / dispatch calls, and
    /// caches its workgroup size for dispatch_threads.
    void bind_pipeline(compute_pipeline const& pipeline);

    /// Binds `group` to descriptor set `set` of the active pipeline. The group's layout must match the
    /// pipeline's for that set.
    void bind_group(int set, binding_group const& group);

    /// Dispatches `x`*`y`*`z` **workgroups** of the active pipeline.
    void dispatch_groups(int x, int y = 1, int z = 1);

    /// Dispatches enough workgroups to cover `x`*`y`*`z` **threads**, rounding up per axis by the bound
    /// pipeline's workgroup size (`ceil(threads / workgroup_size)`). A pipeline must be bound first.
    void dispatch_threads(int x, int y = 1, int z = 1);

    /// Declares per-element access for a *buffer* array / bindless binding, applied to the **next dispatch
    /// only** (declare again before each dispatch that needs it). Scalar bindings have their access inferred
    /// from the shader + bound view; array element usage cannot be — a shader may index only some elements,
    /// or use them differently — so it is declared explicitly here. `binding_name` is the array binding's
    /// reflection name; each `array_buffer_access` names one element and how it is accessed.
    void declare_array_buffer_access(cc::string_view binding_name, cc::span<array_buffer_access const> elements);

    /// Declares per-element access for a *texture* array / bindless binding — like the buffer form (next
    /// dispatch only), but each element also names the layout it must be in.
    void declare_array_texture_access(cc::string_view binding_name, cc::span<array_texture_access const> elements);

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_compute_scope(command_list_compute_scope const&) = delete;
    command_list_compute_scope(command_list_compute_scope&&) = delete;
    command_list_compute_scope& operator=(command_list_compute_scope const&) = delete;
    command_list_compute_scope& operator=(command_list_compute_scope&&) = delete;

private:
    // Only a command list constructs its own scope; the scope in turn reaches the list's protected
    // backend virtuals (mutual friendship).
    friend class command_list;
    explicit command_list_compute_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;

    // Workgroup size of the currently-bound pipeline (defaults to 1s so dispatch_threads == groups
    // until a pipeline is bound). Kept as plain scalars to keep this header dependency-light.
    int _bound_wg_x = 1;
    int _bound_wg_y = 1;
    int _bound_wg_z = 1;
};
} // namespace sg
