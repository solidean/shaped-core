#pragma once

// Single include gate for the D3D12 / DXGI / WRL headers (Windows-sanitized via win32_sanitized)
// plus the shared COM alias and error helper. dx12 TUs include this, not <d3d12.h> & friends.

#include <clean-core/error/result.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/format.hh>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace sg::backend::dx12
{
/// COM smart pointer for D3D12/DXGI object lifetime. Backend-internal — never crosses into sg/sr/sv.
using Microsoft::WRL::ComPtr;

/// Builds a cc::result error from a failed HRESULT, recording the call site (not this helper).
[[nodiscard]] inline auto dx12_error(HRESULT hr,
                                     char const* what,
                                     cc::source_location site = cc::source_location::current())
{
    return cc::error(cc::format("{} (hr=0x{:08X})", what, cc::u32(hr)), site);
}

/// A committed buffer resource together with its persistent CPU mapping.
struct dx12_mapped_buffer
{
    ComPtr<ID3D12Resource> resource;
    void* mapped = nullptr; // byte 0 of the mapping; cast to cc::byte* at the call site
};

/// Creates a `size`-byte committed BUFFER on `heap_type`, left in `initial_state`, and persistently
/// maps it. Used for the inline UPLOAD / READBACK ring buffers. `size` must be > 0.
[[nodiscard]] inline cc::result<dx12_mapped_buffer> create_mapped_ring_buffer(ID3D12Device* device,
                                                                              D3D12_HEAP_TYPE heap_type,
                                                                              D3D12_RESOURCE_STATES initial_state,
                                                                              cc::isize size)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = heap_type;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = UINT64(size);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for buffers

    dx12_mapped_buffer out;
    if (HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
                                                     IID_PPV_ARGS(&out.resource));
        FAILED(hr))
        return dx12_error(hr, "CreateCommittedResource (inline ring buffer) failed");
    if (HRESULT hr = out.resource->Map(0, nullptr, &out.mapped); FAILED(hr))
        return dx12_error(hr, "ID3D12Resource::Map (inline ring buffer) failed");
    return out;
}
} // namespace sg::backend::dx12
