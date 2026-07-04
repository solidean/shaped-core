#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
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
    void bind_group(u32 set, binding_group const& group);

    /// Dispatches `x`*`y`*`z` **workgroups** of the active pipeline.
    void dispatch_groups(u32 x, u32 y, u32 z);

    /// Dispatches enough workgroups to cover `x`*`y`*`z` **threads**, rounding up per axis by the bound
    /// pipeline's workgroup size (`ceil(threads / workgroup_size)`). A pipeline must be bound first.
    void dispatch_threads(u32 x, u32 y, u32 z);

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
    u32 _bound_wg_x = 1;
    u32 _bound_wg_y = 1;
    u32 _bound_wg_z = 1;
};
} // namespace sg
