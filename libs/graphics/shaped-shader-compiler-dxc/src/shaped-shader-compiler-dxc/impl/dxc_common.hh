#pragma once

// Single include gate for the DXC / COM headers (behind clean-core's sanitized win32) plus the
// shared helpers. Internal to the library — never include this from a public header.

#include <clean-core/error/result.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <d3d12shader.h> // ID3D12ShaderReflection (from the Windows SDK)
#include <dxc/dxcapi.h>  // DXC C ABI (from extern/dxc/.install)
#include <wrl/client.h>

#include <string>

namespace ssc::dxc::impl
{
using Microsoft::WRL::ComPtr;

/// Builds a cc::result error from a failed HRESULT, recording the call site (not this helper).
[[nodiscard]] inline auto dxc_error(HRESULT hr,
                                    char const* what,
                                    cc::source_location site = cc::source_location::current())
{
    return cc::error(cc::format("{} (hr=0x{:08X})", what, cc::u32(hr)), site);
}

/// UTF-8 view -> wide string, for DXC's LPCWSTR argument vector.
[[nodiscard]] inline std::wstring to_wide(cc::string_view s)
{
    if (s.empty())
        return {};
    int const n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

/// Wide string -> UTF-8 cc::string (e.g. an include filename DXC hands us).
[[nodiscard]] inline cc::string from_wide(wchar_t const* s)
{
    if (s == nullptr || *s == L'\0')
        return {};
    int const len = int(::wcslen(s));
    int const n = ::WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string tmp(size_t(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, len, tmp.data(), n, nullptr, nullptr);
    return cc::string(tmp.data(), cc::isize(n));
}

/// A source blob kept alive alongside the DxcBuffer that points into it. The buffer is only valid
/// while the returned value lives.
struct source_blob
{
    ComPtr<IDxcBlobEncoding> blob;
    DxcBuffer buffer = {};
};

/// Copies `src` into a DXC UTF-8 blob and wraps it as a DxcBuffer.
[[nodiscard]] inline cc::result<source_blob> make_source_blob(IDxcUtils* utils, cc::string_view src)
{
    source_blob s;
    if (HRESULT hr = utils->CreateBlob(src.data(), UINT32(src.size()), DXC_CP_UTF8, s.blob.GetAddressOf()); FAILED(hr))
        return dxc_error(hr, "IDxcUtils::CreateBlob (source)");
    s.buffer.Ptr = s.blob->GetBufferPointer();
    s.buffer.Size = s.blob->GetBufferSize();
    s.buffer.Encoding = DXC_CP_UTF8;
    return s;
}

/// Reads DXC_OUT_ERRORS as a UTF-8 diagnostic string (empty when there were none).
[[nodiscard]] inline cc::string dxc_diagnostics(IDxcResult* result)
{
    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr)) && errors
        && errors->GetStringLength() > 0)
        return cc::string(errors->GetStringPointer(), cc::isize(errors->GetStringLength()));
    return {};
}
} // namespace ssc::dxc::impl
