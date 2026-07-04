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
/// NOTE: the descriptor range is bump-allocated and not reclaimed yet (it lives until context
/// teardown); the group is also freed immediately on release rather than epoch-deferred. Both are fine
/// while a group is held for the duration of its use. TODO: epoch-deferred free + heap reclaim.
class dx12_binding_group final : public sg::binding_group
{
public:
    [[nodiscard]] static cc::result<dx12_binding_group_handle> create(dx12_context& ctx,
                                                                      dx12_binding_layout_handle layout,
                                                                      cc::span<sg::named_view const> views);

    dx12_binding_layout_handle layout;
    D3D12_GPU_DESCRIPTOR_HANDLE table_start{};
    cc::vector<sg::buffer_handle> referenced; // keeps the bound buffers alive while the group lives
};
} // namespace sg::backend::dx12
