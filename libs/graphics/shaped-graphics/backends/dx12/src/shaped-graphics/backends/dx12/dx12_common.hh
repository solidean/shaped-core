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
} // namespace sg::backend::dx12
