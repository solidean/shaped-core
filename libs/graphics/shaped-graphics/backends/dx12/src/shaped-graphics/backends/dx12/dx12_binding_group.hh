#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// dx12 binding_group: a contiguous range of descriptors in the context's shader-visible heap, one
/// per layout binding, created from the bound views. `table_start` is the GPU handle the command list
/// binds as a root descriptor table.
///
/// The descriptor range comes from the heap region matching `scope`: a persistent group's table is
/// bump-allocated and lives until context teardown; a transient group's is ring-allocated and reclaimed
/// when its epoch retires.
class dx12_binding_group final : public sg::binding_group
{
public:
    [[nodiscard]] static cc::result<dx12_binding_group_handle> create(dx12_context& ctx,
                                                                      dx12_binding_layout_handle layout,
                                                                      cc::span<sg::named_view const> views,
                                                                      sg::lifetime_scope scope);

    dx12_binding_layout_handle layout;
    D3D12_GPU_DESCRIPTOR_HANDLE table_start{};
    cc::vector<sg::buffer_handle> referenced; // keeps the bound buffers alive while the group lives

    // Transient groups expire when their epoch passes: the ring recycles their descriptor slots, so
    // binding one afterwards is a hard error (checked at bind). Both are inert for a persistent group.
    sg::epoch creation_epoch = sg::epoch::invalid;
    bool transient = false;
};
} // namespace sg::backend::dx12
