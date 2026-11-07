#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>

// The D3D12 runtime this process creates its devices through.
//
// Where the Agility SDK was fetched (`SG_HAS_AGILITY`), that is its D3D12Core.dll by default, loaded through
// ID3D12SDKConfiguration1::CreateDeviceFactory: no executable exports D3D12SDKVersion, so a static library can choose it.
// Everywhere else, where the SDK fails to load, and where the system runtime was asked for, it is the runtime Windows carries.
// The choice is made once per process, at the first call of any function here, and never changes after.
// A process that asked for the system runtime never calls CreateDeviceFactory, which on Intel's driver is what costs every
// later process-wide shader compile its cache (docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels).
namespace sg::backend::dx12
{
enum class d3d12_runtime : u8
{
    system,
    agility, ///< the system runtime where this build has no Agility SDK
};

/// Asks for a runtime before this process uses one; it wins over `SC_DX12_RUNTIME`.
/// It must be called before any device, debug interface or feature query, and a later call must ask for the same runtime.
void choose_d3d12_runtime(d3d12_runtime runtime);

/// The runtime this process asked for: `choose_d3d12_runtime`'s, else `SC_DX12_RUNTIME` (`system` or `agility`), else
/// the Agility SDK's where it was fetched.
/// Calling it settles the choice.
[[nodiscard]] d3d12_runtime requested_d3d12_runtime();

/// Whether this process's devices come from the Agility SDK's runtime rather than the system's.
/// False where the Agility SDK was asked for and failed to load.
[[nodiscard]] bool runs_agility_runtime();

/// The policy every test binary of ours applies at startup: the system runtime on an Intel GPU unless `SC_DX12_RUNTIME`
/// names one, the default everywhere else.
/// It reads the adapter through DXGI alone, so it never loads a D3D12 runtime itself.
void choose_test_d3d12_runtime();

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
