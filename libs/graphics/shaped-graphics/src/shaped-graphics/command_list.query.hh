#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/gpu_timestamp.hh>

namespace sg
{
/// GPU-query recording facade for a command list, reached as `cmd.query`: record queries (currently
/// GPU timestamps) at this point in the list and read their results back on the host after submit.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_query_scope
{
public:
    /// Whether this backend/device supports GPU timestamps. When false, record_gpu_timestamp() returns
    /// an invalid query (is_valid() == false). dx12 supports them on the direct queue; a backend without
    /// timestamp support returns false.
    [[nodiscard]] bool is_supported() const;

    /// Records a point-in-time GPU timestamp at this point in the list. Returns a gpu_timestamp whose tick
    /// becomes readable once the submitted list has finished on the GPU (block/poll via the timestamp).
    /// Returns an invalid timestamp when timestamps are unsupported (see is_supported()). Only differences
    /// between two timestamps are meaningful.
    [[nodiscard]] gpu_timestamp record_gpu_timestamp();

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_query_scope(command_list_query_scope const&) = delete;
    command_list_query_scope(command_list_query_scope&&) = delete;
    command_list_query_scope& operator=(command_list_query_scope const&) = delete;
    command_list_query_scope& operator=(command_list_query_scope&&) = delete;

private:
    // Only a command list constructs its own scope; the scope in turn reaches the list's protected
    // backend virtuals (mutual friendship).
    friend class command_list;
    explicit command_list_query_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
} // namespace sg
