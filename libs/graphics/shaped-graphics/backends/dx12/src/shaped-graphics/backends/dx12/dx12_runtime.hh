#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>

// The D3D12 runtime this process creates its devices through.
//
// Where the Agility SDK was fetched (`SG_HAS_AGILITY`), that is its D3D12Core.dll, loaded through
// ID3D12SDKConfiguration1::CreateDeviceFactory: no executable exports D3D12SDKVersion, so a static library can choose it.
// Everywhere else, and where the SDK fails to load, it is the runtime Windows carries.
// The choice is made once per process, at the first call of any function here, and never changes after.
namespace sg::backend::dx12
{
/// Whether this process's devices come from the Agility SDK's runtime rather than the system's.
[[nodiscard]] bool runs_agility_runtime();

/// Whether acceleration-structure buffers carry `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE`, which a
/// micromap array needs: from the first context that reports `sg::feature::opacity_micromap` on, and never before.
/// A device without micromaps so keeps the creation calls it always had.
[[nodiscard]] bool marks_acceleration_structures();
void mark_acceleration_structures();

/// `D3D12CreateDevice` through the runtime this process chose; a null `out` probes as `D3D12CreateDevice` does.
/// One adapter yields one device, as the system's singleton does.
HRESULT create_d3d12_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out);

/// `D3D12GetDebugInterface` through the runtime this process chose: `ID3D12Debug` for `CLSID_D3D12Debug`, and the DRED
/// settings for `CLSID_D3D12DeviceRemovedExtendedData`.
HRESULT get_d3d12_configuration(REFCLSID clsid, REFIID riid, void** out);

/// `CheckFeatureSupport` as the runtime that made `device` wants it asked: through `CheckFeatureSupport1` with the SDK
/// version the backend was built against on the Agility SDK's, so no capability reported exceeds what that version knew.
HRESULT check_feature_support(ID3D12Device* device, D3D12_FEATURE feature, void* data, UINT size);
} // namespace sg::backend::dx12
