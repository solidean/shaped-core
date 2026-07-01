#pragma once

// The single include gate for the DirectX 12 / DXGI / WRL headers. Every dx12 backend TU includes
// this instead of reaching for <d3d12.h> & friends directly, so the Windows-header sanitization
// (via clean-core's win32_sanitized) and the shared COM/error helpers live in one place.

#include <clean-core/error/result.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/format.hh>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace sg::backend::dx12
{
/// COM smart pointer used throughout the backend for D3D12/DXGI object lifetime. Backend-internal:
/// these types never cross into sg/sr/sv, so the std::-adjacent WRL dependency stays contained.
using Microsoft::WRL::ComPtr;

/// Builds a cc::result error from a failed HRESULT. The source location defaults to the call site,
/// so the recorded site points at the failing D3D12 call, not at this helper.
[[nodiscard]] inline auto dx12_error(HRESULT hr,
                                     char const* what,
                                     cc::source_location site = cc::source_location::current())
{
    return cc::error(cc::format("{} (hr=0x{:08X})", what, cc::u32(hr)), site);
}
} // namespace sg::backend::dx12
