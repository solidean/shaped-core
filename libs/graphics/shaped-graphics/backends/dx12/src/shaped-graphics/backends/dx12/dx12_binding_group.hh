#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_descriptor_heap.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/views.hh> // sg::view_class

namespace sg::backend::dx12
{
/// A bound buffer paired with the access class it is used as — the backend-typed input to the dispatch
/// hazard declares (see dx12_command_list::compute_dispatch).
struct dx12_hazard_view
{
    dx12_buffer_const_handle buffer;
    sg::view_class access;
};

/// dx12 binding_group: a contiguous range of descriptors in the context's shader-visible heap, one
/// per layout binding, created from the bound views. `table_start` is the GPU handle the command list
/// binds as a root descriptor table.
///
/// The descriptor range comes from the heap region matching `scope`: a persistent group's table is
/// allocated from the free list and returned to it (epoch-deferred) when the group is released; a
/// transient group's is ring-allocated and reclaimed collectively when its epoch retires.
class dx12_binding_group final : public sg::binding_group
{
public:
    [[nodiscard]] static cc::result<dx12_binding_group_handle> create(dx12_context& ctx,
                                                                      dx12_binding_layout_handle layout,
                                                                      cc::span<sg::named_view const> views,
                                                                      sg::lifetime_scope scope);

    dx12_binding_group() = default;

    // Returns a persistent group's descriptor range to the free list, deferred until its last-using
    // epoch retires (a transient group's range is reclaimed by the ring, so nothing to free). Body in .cc.
    ~dx12_binding_group() override;

    dx12_context* _ctx = nullptr; // creating context — outlives this group (for the deferred free)
    dx12_binding_layout_handle layout;
    D3D12_GPU_DESCRIPTOR_HANDLE table_start{};
    dx12_descriptor_alloc table; // the group's descriptor range (its start feeds table_start; count for freeing)
    cc::vector<dx12_buffer_const_handle> referenced; // keeps the bound buffers alive while the group lives
    cc::vector<dx12_hazard_view> hazard_views;       // (buffer + access class) — declared for hazards at dispatch

    // Transient groups expire when their epoch passes: the ring recycles their descriptor slots, so
    // binding one afterwards is a hard error (checked at bind). Both are inert for a persistent group.
    sg::epoch creation_epoch = sg::epoch::invalid;
    bool transient = false;
};
} // namespace sg::backend::dx12
