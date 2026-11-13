#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/conversion.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_runtime.hh>
#include <shaped-graphics/context/gpu_selector.hh>

#ifndef SG_HAS_AGILITY
#error "SG_HAS_AGILITY must be defined by the dx12 backend's CMake"
#endif
#ifndef SG_HAS_AGILITY_PREVIEW
#error "SG_HAS_AGILITY_PREVIEW must be defined by the dx12 backend's CMake"
#endif

namespace sg::backend::dx12
{
namespace
{
#if SG_HAS_AGILITY
// The version the runtime is loaded and asked as: a preview runtime is the preview SDK's, not the retail one it builds on.
#if SG_HAS_AGILITY_PREVIEW
constexpr UINT k_sdk_version = D3D12_PREVIEW_SDK_VERSION;
#else
constexpr UINT k_sdk_version = D3D12_SDK_VERSION;
#endif

cc::atomic<bool> g_raytracing_preview = false;

/// Enables the preview runtime's raytracing features on `factory`, before it makes any device.
/// A refusal leaves the factory as it was.
void enable_raytracing_preview([[maybe_unused]] ID3D12DeviceFactory* factory)
{
#if SG_HAS_AGILITY_PREVIEW
    UUID const features[] = {D3D12ExperimentalShaderModels, D3D12RaytracingPreview};
    if (HRESULT hr
        = factory->EnableExperimentalFeatures(UINT(sizeof(features) / sizeof(UUID)), features, nullptr, nullptr);
        FAILED(hr))
    {
        CC_LOG_INFO("the preview Agility SDK refused its experimental features (hr=0x{:08X}), so clustered geometry "
                    "and "
                    "partitioned TLAS report absent",
                    u32(hr));
        return;
    }
    g_raytracing_preview.store(true, cc::memory_order_release);
#endif
}

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
        = configuration->CreateDeviceFactory(k_sdk_version, directory.c_str_materialize(), IID_PPV_ARGS(&factory));
        FAILED(hr))
    {
        CC_LOG_WARNING("the Agility SDK {} in {} did not load (hr=0x{:08X}), so dx12 runs on the system runtime{}",
                       k_sdk_version, directory, u32(hr),
                       SG_HAS_AGILITY_PREVIEW ? ": a preview runtime loads only with Windows Developer Mode on" : "");
        return nullptr;
    }
    enable_raytracing_preview(factory.Get());
    return factory;
}

ID3D12DeviceFactory* agility_factory()
{
    static auto const factory = requested_d3d12_runtime() == d3d12_runtime::agility ? make_agility_factory() : nullptr;
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

namespace
{
cc::atomic<bool> g_marks_acceleration_structures = false;
} // namespace

namespace sg::backend::dx12
{
namespace
{
// What choose_d3d12_runtime asked for, and whether requested_d3d12_runtime has settled it.
struct runtime_choice
{
    cc::optional<d3d12_runtime> chosen;
    cc::optional<d3d12_runtime> settled;
};
cc::mutex<runtime_choice> g_runtime_choice;

constexpr UINT k_intel_vendor_id = 0x8086;
constexpr UINT k_microsoft_vendor_id = 0x1414;

/// The vendor of the hardware adapter a context's own search takes first, `SC_GPU` included; 0 where there is none.
/// It cannot ask the adapter for D3D12 support as that search does, since asking loads a runtime.
[[nodiscard]] UINT selected_hardware_vendor_id()
{
    auto const selector = sg::gpu_selector_from_environment();
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        auto desc = DXGI_ADAPTER_DESC1{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == k_microsoft_vendor_id)
            continue;
        auto const capacity = isize(sizeof(desc.Description) / sizeof(desc.Description[0]));
        auto length = isize(0);
        while (length < capacity && desc.Description[length] != L'\0')
            ++length;
        auto const name
            = cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(desc.Description), length));
        if (!sg::gpu_selector_matches(selector, u32(desc.VendorId), name))
            continue;
        return desc.VendorId;
    }
    return 0;
}

[[nodiscard]] d3d12_runtime default_runtime()
{
    if (auto const value = cc::environment_variable("SC_DX12_RUNTIME"); value.has_value())
    {
        if (value.value() == "system")
            return d3d12_runtime::system;
        if (value.value() == "agility")
            return d3d12_runtime::agility;
        CC_LOG_WARNING("SC_DX12_RUNTIME='{}' is ignored: the accepted values are 'system' and 'agility'", value.value());
    }
    if (!SG_HAS_AGILITY)
        return d3d12_runtime::system;
    // TEMPORARY(debt-sg-intel-keeps-off-agility): Intel's driver compiles every shader of an Agility process anew.
    if (selected_hardware_vendor_id() == k_intel_vendor_id)
    {
        CC_LOG_INFO("dx12 runs on the system runtime on this Intel GPU, without SM 6.9, shader execution reordering "
                    "and DXR 1.2: SC_DX12_RUNTIME=agility asks for the Agility SDK anyway");
        return d3d12_runtime::system;
    }
    return d3d12_runtime::agility;
}

} // namespace
} // namespace sg::backend::dx12

void sg::backend::dx12::choose_d3d12_runtime(d3d12_runtime runtime)
{
    g_runtime_choice.lock(
        [&](runtime_choice& c)
        {
            CC_ASSERT(!c.settled.has_value() || c.settled.value() == runtime,
                      "the D3D12 runtime was settled already, as another one: choose it before any device");
            CC_ASSERT(!c.chosen.has_value() || c.chosen.value() == runtime, "the D3D12 runtime was chosen already, as "
                                                                            "another one");
            c.chosen = runtime;
        });
}

sg::backend::dx12::d3d12_runtime sg::backend::dx12::requested_d3d12_runtime()
{
    return g_runtime_choice.lock(
        [](runtime_choice& c)
        {
            if (!c.settled.has_value())
                c.settled = c.chosen.has_value() ? c.chosen.value() : default_runtime();
            return c.settled.value();
        });
}

bool sg::backend::dx12::marks_acceleration_structures()
{
    return g_marks_acceleration_structures.load(cc::memory_order_acquire);
}

void sg::backend::dx12::mark_acceleration_structures()
{
    g_marks_acceleration_structures.store(true, cc::memory_order_release);
}

bool sg::backend::dx12::runs_agility_runtime()
{
#if SG_HAS_AGILITY
    return agility_factory() != nullptr;
#else
    return false;
#endif
}

bool sg::backend::dx12::runs_raytracing_preview()
{
#if SG_HAS_AGILITY
    return runs_agility_runtime() && g_raytracing_preview.load(cc::memory_order_acquire);
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
        return device16->CheckFeatureSupport1(feature, data, size, k_sdk_version);
#endif
    return device->CheckFeatureSupport(feature, data, size);
}
