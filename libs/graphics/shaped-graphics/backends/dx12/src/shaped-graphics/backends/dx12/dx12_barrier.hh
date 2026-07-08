#pragma once

#include <shaped-graphics/backend/resource_access_state.hh>
#include <shaped-graphics/backend/subresource.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>

/// dx12's barrier layer: translate the backend-neutral access vocabulary (stages / access / layout) into
/// D3D12 enhanced-barrier bits and record a batched buffer or texture barrier. dx12 owns its barriering
/// entirely — the core only hands it the `access_barrier` computed by the shared `resource_access_state`
/// machine.

namespace sg::backend::dx12
{
/// The D3D12 enhanced-barrier sync scope for a set of pipeline stages.
[[nodiscard]] D3D12_BARRIER_SYNC d3d12_sync_from(sg::pipeline_stage_flags stages);

/// The D3D12 enhanced-barrier access bits for a set of accesses.
[[nodiscard]] D3D12_BARRIER_ACCESS d3d12_access_from(sg::access_flags access);

/// The D3D12 barrier layout for a texture_layout. Buffers never call this (they have no layout).
[[nodiscard]] D3D12_BARRIER_LAYOUT d3d12_layout_from(sg::texture_layout layout);

/// Records one enhanced buffer barrier for `b` over the whole `resource` (D3D12 buffer barriers cover
/// the entire resource). Requires enhanced-barrier support (ID3D12GraphicsCommandList7). Only call when
/// `b.needed`.
void emit_buffer_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, sg::access_barrier const& b);

/// Records one enhanced texture barrier for `b` scoped to `range` (mip × array-slice × aspect-plane) of
/// `resource`, including the layout transition `b.src_layout → b.dst_layout`. An `undefined` source layout
/// emits with DISCARD (contents are not preserved). Only call when `b.needed`.
void emit_texture_barrier(ID3D12GraphicsCommandList* list,
                          ID3D12Resource* resource,
                          sg::subresource_range const& range,
                          sg::access_barrier const& b);
} // namespace sg::backend::dx12
