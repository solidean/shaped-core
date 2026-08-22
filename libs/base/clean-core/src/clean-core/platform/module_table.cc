#include "module_table.hh"

#include <clean-core/string/format.hh>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <clean-core/platform/win32_sanitized.hh>
#include <tlhelp32.h> // the module snapshot, which <Windows.h> does not pull in under LEAN_AND_MEAN
#endif

using namespace cc::primitive_defines;

namespace
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
constexpr bool has_module_enumeration = true;

/// The symbol-server key for a mapped PE: its link timestamp and its image size, as hex.
///
/// Read from the headers at the mapped base rather than from the file, because the module is right there and opening
/// the file would be a second way for this to fail.
/// Empty for anything that is not a PE, which is how a consumer learns not to trust a match on path alone.
[[nodiscard]] cc::string pe_identity(u64 base)
{
    auto const* const dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return {};

    auto const* const nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(base + u64(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return {};

    // Exactly the two fields a symbol server URL is built from, in exactly that order and case.
    return cc::format("{:08X}{:x}", u32(nt->FileHeader.TimeDateStamp), u32(nt->OptionalHeader.SizeOfImage));
}
#else
constexpr bool has_module_enumeration = false;
#endif
} // namespace

cc::string_view cc::loaded_module::name() const
{
    for (auto i = path.size(); i > 0; --i)
        if (path[i - 1] == '\\' || path[i - 1] == '/')
            return cc::string_view(path).subview(i);
    return path;
}

bool cc::module_enumeration_available()
{
    return has_module_enumeration;
}

cc::vector<cc::loaded_module> cc::enumerate_loaded_modules()
{
    cc::vector<cc::loaded_module> out;

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    auto* const snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return out;

    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (::Module32First(snap, &entry) != 0)
        do
        {
            auto const base = reinterpret_cast<u64>(entry.modBaseAddr);
            out.push_back({
                .base = base,
                .size = u64(entry.modBaseSize),
                .path = cc::string(entry.szExePath),
                .identity = pe_identity(base),
            });
        } while (::Module32Next(snap, &entry) != 0);

    ::CloseHandle(snap);
#endif

    return out;
}
