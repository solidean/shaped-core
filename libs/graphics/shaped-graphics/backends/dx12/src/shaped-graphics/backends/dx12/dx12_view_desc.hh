#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
class dx12_tlas;

/// Creates the native D3D12 view (CBV / SRV / UAV) for buffer `view` into the CPU descriptor slot
/// `dst`, dispatching on (view.access, view.shape). The view's buffer must be a dx12_buffer. This is
/// the point where the erased sg::raw_view becomes a concrete backend descriptor.
void create_buffer_view(ID3D12Device* device, sg::raw_view const& view, D3D12_CPU_DESCRIPTOR_HANDLE dst);

/// Creates the native D3D12 acceleration-structure SRV for `tlas` into `dst`. Unique among views: the
/// resource argument to CreateShaderResourceView is null and the descriptor is addressed purely by the
/// TLAS's storage GPU virtual address (the AS location).
void create_accel_view(ID3D12Device* device, dx12_tlas const& tlas, D3D12_CPU_DESCRIPTOR_HANDLE dst);

/// Creates the native D3D12 texture view (SRV for readonly, UAV for readwrite) for `view` into the CPU
/// descriptor slot `dst`. The view's texture must be a dx12_texture; the SRV/UAV dimension, mip/array/plane
/// range, and 3D W-slice window all come from the view. depth-as-SRV is unsupported.
void create_texture_view(ID3D12Device* device, sg::raw_view const& view, D3D12_CPU_DESCRIPTOR_HANDLE dst);
} // namespace sg::backend::dx12
