#pragma once

#include <shaped-graphics/backends/dx12/fwd.hh>

namespace sg::backend::dx12
{
struct dx12_track_result;
} // namespace sg::backend::dx12

/// What tracking one access of a buffer or texture found, in the single lock that recorded it.
/// The command list files the resource by these: once per op for the pre-op flush, once per list for submit.
struct sg::backend::dx12::dx12_track_result
{
    /// The first access of the resource in the current op, so the command list enqueues it for the pre-op flush.
    bool first_this_op = false;

    /// The first access of the resource in this command list, so it joins the list's touched set, finalized at submit.
    bool first_this_list = false;

    /// The first write of the resource in this command list, so it joins the written set, whose download waits submit folds.
    bool first_write_this_list = false;

    /// The resource is an array member of this list, one an array application tracked, and this access landed in the write bucket.
    /// The command list counts it, which is what ends every current array application.
    bool member_write = false;

    /// Before this access, the current op already held a write or a pending layout change of the resource.
    bool held_write = false;
};
