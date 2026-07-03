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
    /// Makes `pipeline` the active compute pipeline for subsequent bind_group / dispatch calls.
    void bind_pipeline(compute_pipeline const& pipeline);

    /// Binds `group` to descriptor set `set` of the active pipeline. The group's layout must match the
    /// pipeline's for that set.
    void bind_group(u32 set, binding_group const& group);

    /// Dispatches `x`*`y`*`z` workgroups of the active pipeline. A pipeline (and the groups it needs)
    /// must be bound first.
    void dispatch(u32 x, u32 y, u32 z);

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
};
} // namespace sg
