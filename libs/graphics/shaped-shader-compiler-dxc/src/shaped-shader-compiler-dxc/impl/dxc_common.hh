#pragma once

// Single include gate for the DXC / COM headers (behind clean-core's sanitized win32) plus the shared helpers.
// Internal to the library — never include this from a public header.

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-compiler-dxc/fwd.hh> // also what puts the bare sized aliases in scope inside ssc::dxc

#include <string>

// The DXC / COM headers reach <rpcndr.h> through <unknwn.h>, which WIN32_LEAN_AND_MEAN does not stop, so the `byte` rename is repeated over them here.
// See win32_sanitized.hh for why it is needed.
// <string> stays above the bracket: a C++ header parsed under the macro would lose `std::byte`.
//
// Off Windows there is no windows.h to sanitize and no rename to make: dxcapi.h pulls in DXC's own WinAdapter.h,
// which supplies HRESULT, IUnknown and IID_PPV_ARGS itself.
// d3d12shader.h is Windows-only in both senses — the Linux release ships no copy, and the reflection interfaces it
// declares are not implemented there, which is why the SPIR-V path reflects the emitted module instead.
#ifdef CC_OS_WINDOWS
#define byte win_byte_override
#include <d3d12shader.h> // ID3D12ShaderReflection (from the Windows SDK)
#include <dxc/dxcapi.h>  // DXC C ABI (from extern/dxc/.install)
#undef byte
#else
#include <dxc/dxcapi.h>
#endif

#include <shaped-shader-compiler-dxc/impl/com_ptr.hh>

namespace ssc::dxc::impl
{
// One COM pointer on both platforms; see com_ptr.hh for why this is not WRL's.
template <class T>
using ComPtr = com_ptr<T>;

/// Builds a cc::result error from a failed HRESULT, recording the call site (not this helper).
[[nodiscard]] inline auto dxc_error(HRESULT hr,
                                    char const* what,
                                    cc::source_location site = cc::source_location::current())
{
    return cc::error(cc::format("{} (hr=0x{:08X})", what, u32(hr)), site);
}

/// UTF-8 view -> wide string, for DXC's LPCWSTR argument vector.
///
/// `wchar_t` is UTF-16 on Windows and UTF-32 elsewhere, and DXC's WinAdapter types LPCWSTR as `wchar_t const*` either
/// way — so the width, not the platform, is what decides the encoding.
/// The UTF-8 half is cc::utf8_to_utf16's rather than a second decoder written here; only the surrogate recombination
/// for a 4-byte wchar_t is local, since cc has no utf8-to-utf32 today.
[[nodiscard]] inline std::wstring to_wide(cc::string_view s)
{
    if (s.empty())
        return {};

    auto const u16 = cc::utf8_to_utf16(s);
    std::wstring w;
    w.reserve(u16.size());

    if constexpr (sizeof(wchar_t) == 2)
    {
        for (auto unit : u16)
            w.push_back(wchar_t(unit));
    }
    else
    {
        for (isize i = 0; i < isize(u16.size()); ++i)
        {
            char16_t const unit = u16[i];
            bool const is_high = unit >= 0xD800 && unit <= 0xDBFF;
            if (is_high && i + 1 < isize(u16.size()) && u16[i + 1] >= 0xDC00 && u16[i + 1] <= 0xDFFF)
            {
                // A surrogate pair is one code point once wchar_t is wide enough to hold it.
                w.push_back(wchar_t(0x10000 + ((char32_t(unit) - 0xD800) << 10) + (char32_t(u16[i + 1]) - 0xDC00)));
                ++i;
            }
            else
                w.push_back(wchar_t(unit));
        }
    }
    return w;
}

/// Wide string -> UTF-8 cc::string (e.g. an include filename DXC hands us).
[[nodiscard]] inline cc::string from_wide(wchar_t const* s)
{
    if (s == nullptr || *s == L'\0')
        return {};

    cc::vector<char16_t> u16;
    for (wchar_t const* p = s; *p != L'\0'; ++p)
    {
        auto const cp = char32_t(*p);
        if constexpr (sizeof(wchar_t) == 2)
            u16.push_back(char16_t(cp));
        else if (cp >= 0x10000)
        {
            // Split an astral code point back into the pair cc::utf16_to_utf8 expects.
            auto const v = cp - 0x10000;
            u16.push_back(char16_t(0xD800 + (v >> 10)));
            u16.push_back(char16_t(0xDC00 + (v & 0x3FF)));
        }
        else
            u16.push_back(char16_t(cp));
    }
    return cc::utf16_to_utf8(u16);
}

/// A source blob kept alive alongside the DxcBuffer that points into it.
/// The buffer is only valid while the returned value lives.
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
        return cc::string(errors->GetStringPointer(), isize(errors->GetStringLength()));
    return {};
}
} // namespace ssc::dxc::impl
