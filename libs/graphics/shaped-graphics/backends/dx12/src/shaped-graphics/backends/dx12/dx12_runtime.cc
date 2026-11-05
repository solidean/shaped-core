#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_runtime.hh>

#ifndef SG_HAS_AGILITY
#error "SG_HAS_AGILITY must be defined by the dx12 backend's CMake"
#endif

namespace sg::backend::dx12
{
namespace
{
#if SG_HAS_AGILITY
/// Where D3D12Core.dll is: a `D3D12` directory beside the executable where one was staged there, which is how a shipped
/// application carries it, and the fetched install otherwise.
cc::string agility_directory()
{
    wchar_t module[MAX_PATH] = {};
    auto const length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        auto directory = cc::string();
        for (auto i = DWORD(0); i < length; ++i)
            directory += char(module[i] < 128 ? module[i] : '?');
        while (!directory.empty() && directory.back() != '\\' && directory.back() != '/')
            directory.resize_down_to(directory.size() - 1);
        directory += "D3D12\\";
        auto dll = directory + "D3D12Core.dll";
        if (GetFileAttributesA(dll.c_str_materialize()) != INVALID_FILE_ATTRIBUTES)
            return directory;
    }
    return cc::string(SG_AGILITY_RUNTIME_DIR);
}

/// The Agility SDK's device factory, or null where it did not load.
ComPtr<ID3D12DeviceFactory> make_agility_factory()
{
    ComPtr<ID3D12SDKConfiguration1> configuration;
    if (HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration)); FAILED(hr))
    {
        CC_LOG_WARNING("the D3D12 runtime has no SDK configuration (hr=0x{:08X}), so dx12 runs on the system runtime",
                       u32(hr));
        return nullptr;
    }
    auto directory = agility_directory();
    ComPtr<ID3D12DeviceFactory> factory;
    if (HRESULT hr
        = configuration->CreateDeviceFactory(D3D12_SDK_VERSION, directory.c_str_materialize(), IID_PPV_ARGS(&factory));
        FAILED(hr))
    {
        CC_LOG_WARNING("the Agility SDK {} in {} did not load (hr=0x{:08X}), so dx12 runs on the system runtime",
                       D3D12_SDK_VERSION, directory, u32(hr));
        return nullptr;
    }
    return factory;
}

ID3D12DeviceFactory* agility_factory()
{
    static auto const factory = make_agility_factory();
    return factory.Get();
}

/// The one device each adapter has, by LUID, as the system runtime's D3D12CreateDevice keeps it.
/// The factory's ALLOW_RETURNING_EXISTING_DEVICE gives a second device on a second call regardless, and every context of
/// the process sharing one is what the backend's message routing rests on (dx12_context::set_message_callback).
/// Held for the life of the process, and replaced once it reports removal, so a reset GPU gets a fresh device.
HRESULT shared_agility_device(ID3D12DeviceFactory* factory,
                              IUnknown* adapter,
                              D3D_FEATURE_LEVEL level,
                              REFIID riid,
                              void** out)
{
    ComPtr<IDXGIAdapter1> dxgi;
    auto desc = DXGI_ADAPTER_DESC1{};
    if (out == nullptr || adapter == nullptr || FAILED(adapter->QueryInterface(IID_PPV_ARGS(&dxgi)))
        || FAILED(dxgi->GetDesc1(&desc)))
        return factory->CreateDevice(adapter, level, riid, out);
    struct shared
    {
        LUID luid;
        D3D_FEATURE_LEVEL level;
        ComPtr<ID3D12Device> device;
    };
    static cc::mutex<cc::vector<shared>> devices;
    return devices.lock(
        [&](cc::vector<shared>& all) -> HRESULT
        {
            for (auto& d : all)
                if (d.luid.LowPart == desc.AdapterLuid.LowPart && d.luid.HighPart == desc.AdapterLuid.HighPart
                    && d.level == level)
                {
                    if (d.device->GetDeviceRemovedReason() == S_OK)
                        return d.device->QueryInterface(riid, out);
                    d.device = nullptr;
                }
            ComPtr<ID3D12Device> device;
            if (HRESULT hr = factory->CreateDevice(adapter, level, IID_PPV_ARGS(&device)); FAILED(hr))
                return hr;
            auto replaced = false;
            for (auto& d : all)
                if (d.device == nullptr && d.luid.LowPart == desc.AdapterLuid.LowPart
                    && d.luid.HighPart == desc.AdapterLuid.HighPart && d.level == level)
                {
                    d.device = device;
                    replaced = true;
                }
            if (!replaced)
                all.push_back({.luid = desc.AdapterLuid, .level = level, .device = device});
            return device->QueryInterface(riid, out);
        });
}
#endif
} // namespace
} // namespace sg::backend::dx12

bool sg::backend::dx12::runs_agility_runtime()
{
#if SG_HAS_AGILITY
    return agility_factory() != nullptr;
#else
    return false;
#endif
}

HRESULT sg::backend::dx12::create_d3d12_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out)
{
#if SG_HAS_AGILITY
    if (auto* const factory = agility_factory())
        return shared_agility_device(factory, adapter, level, riid, out);
#endif
    return D3D12CreateDevice(adapter, level, riid, out);
}

HRESULT sg::backend::dx12::get_d3d12_configuration(REFCLSID clsid, REFIID riid, void** out)
{
#if SG_HAS_AGILITY
    if (auto* const factory = agility_factory())
        return factory->GetConfigurationInterface(clsid, riid, out);
#endif
    (void)clsid;
    return D3D12GetDebugInterface(riid, out);
}

HRESULT sg::backend::dx12::check_feature_support(ID3D12Device* device, D3D12_FEATURE feature, void* data, UINT size)
{
#if SG_HAS_AGILITY
    if (ComPtr<ID3D12Device16> device16;
        runs_agility_runtime() && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device16))))
        return device16->CheckFeatureSupport1(feature, data, size, D3D12_SDK_VERSION);
#endif
    return device->CheckFeatureSupport(feature, data, size);
}
