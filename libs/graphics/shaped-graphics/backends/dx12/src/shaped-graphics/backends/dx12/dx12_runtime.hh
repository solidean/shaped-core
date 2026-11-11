#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

// The D3D12 runtime this process creates its devices through, one per process and settled at the first call here.
//
// Where the Agility SDK was fetched (`SG_HAS_AGILITY`), the default is its D3D12Core.dll.
// It loads through ID3D12SDKConfiguration1::CreateDeviceFactory, since no executable exports D3D12SDKVersion.
// Everywhere else, where the SDK fails to load, and where the system runtime was asked for, it is the one Windows carries.
// A process on the system runtime never creates the SDK's device factory.
// On Intel's driver that factory alone costs the process every shader cache hit, vulkan's included.
// docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels has the reproduction.
enum class sg::backend::dx12::d3d12_runtime : sg::u8
{
    system,
    agility, ///< the system runtime where this build has no Agility SDK
};

namespace sg::backend::dx12
{

/// Asks for a runtime before this process uses one; it wins over `SC_DX12_RUNTIME`.
/// It must be called before any device, debug interface or feature query, and a later call must ask for the same runtime.
void choose_d3d12_runtime(d3d12_runtime runtime);

/// The runtime this process asked for, which calling it settles.
/// That is `choose_d3d12_runtime`'s, else `SC_DX12_RUNTIME` (`system` or `agility`), else the Agility SDK's where fetched.
/// The default is the system runtime where the first hardware adapter is Intel's, read through DXGI alone.
[[nodiscard]] d3d12_runtime requested_d3d12_runtime();

/// Whether this process's devices come from the Agility SDK's runtime rather than the system's.
/// False where the Agility SDK was asked for and failed to load.
[[nodiscard]] bool runs_agility_runtime();

/// Whether this process's runtime is the pre-release Agility SDK with its preview raytracing features enabled.
/// Only a build with `SG_HAS_AGILITY_PREVIEW` asks for them, and that runtime loads only with Windows Developer Mode on.
/// Where they were refused the runtime still loads, and the features they carry report absent.
[[nodiscard]] bool runs_raytracing_preview();

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
