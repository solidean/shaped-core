#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // and the D3D12 headers under it

// What EnableDebugLayer does to an adapter when it is called too late, in raw D3D12 with no sg in the way.
//
// The debug layer is a PROCESS switch: D3D12CreateDevice consults it at creation time, devices made before it are
// never retrofitted, and nothing turns it off again.
// The documented contract is therefore that it precedes the process's first device.
// This test breaks that contract on purpose and then asks the adapter how it feels about it.
//
// On an NVIDIA RTX 5070 Laptop GPU (driver 32.0.16.1692) the answer is DXGI_ERROR_DEVICE_RESET (0x887A0007):
// the adapter is reset by the late arming, stays reset for seconds, and every D3D12CreateDevice on it fails
// meanwhile -- while the GPU itself is healthy and the AMD adapter beside it is untouched.
// That is what made this expensive to find: the failure surfaces at the NEXT context creation, which looks like a
// broken GPU rather than like something this process did to it.
//
// On an affected driver this test FAILS, and that failure is the demonstration -- it asserts the behaviour a driver
// respecting the contract would show.
//
// MANUAL, and it has to be: passing leaves the process with a debug layer nobody asked for, and failing leaves the
// adapter reset, so every GPU test after it in the same binary would fail for reasons of this test's making.
// That is also why the refusal in sg::create_dx12_context has no automatic test: observing it needs a process where
// the layer was never armed, and every dx12 test binary arms it in its entry driver.
//
//   uv run dev.py test "sg dx12 - late EnableDebugLayer" --manual

using namespace cc::primitive_defines;
using Microsoft::WRL::ComPtr;

namespace
{
/// The first non-software adapter, or null when the host has none.
ComPtr<IDXGIAdapter1> first_hardware_adapter(IDXGIFactory4* factory)
{
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            return adapter;
    }
    return nullptr;
}

/// Whether the adapter can still produce a device, without creating one.
/// A null out-param makes D3D12CreateDevice a support query: S_FALSE means yes.
HRESULT probe(IDXGIAdapter1* adapter)
{
    return D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
}
} // namespace

TEST("sg dx12 - late EnableDebugLayer resets the adapter", nx::config::manual)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        SKIP("no DXGI");

    auto const adapter = first_hardware_adapter(factory.Get());
    if (adapter == nullptr)
        SKIP("no hardware adapter");

    // 1. Healthy to begin with, which is the premise the rest of the test rests on.
    REQUIRE(SUCCEEDED(probe(adapter.Get())));

    // 2. A device, created WITHOUT the debug layer -- the step that closes the window.
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        SKIP("the adapter has no D3D12 device to give");

    // Still healthy: creating the device is not what does the damage.
    CHECK(SUCCEEDED(probe(adapter.Get())));

    // 3. There is no per-device way to ask for validation instead, which is what makes the process switch the only
    // lever: ID3D12DebugDevice is obtained FROM a device that already has the layer, and tunes it rather than
    // turning it on.
    // So a device created without the layer cannot be given one, and this QueryInterface is expected to fail.
    ComPtr<ID3D12DebugDevice> debug_device;
    CHECK(FAILED(device->QueryInterface(IID_PPV_ARGS(&debug_device))));

    // 4. And now the contract violation, with the device above still alive.
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        SKIP("no debug layer on this host (the Graphics Tools feature is not installed)");
    debug->EnableDebugLayer();

    // 5. The observation.
    // Nothing was submitted and nothing faulted, and the adapter answers this way anyway.
    auto const after = probe(adapter.Get());
    CHECK(SUCCEEDED(after))
        .context(cc::format("the adapter stopped answering after a late EnableDebugLayer: 0x{:08X} "
                            "(0x887A0007 is DXGI_ERROR_DEVICE_RESET)",
                            u32(after)));
}
