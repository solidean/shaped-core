#include "module_table.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/string/format.hh>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <clean-core/platform/win32_sanitized.hh>
#include <tlhelp32.h> // the module snapshot, which <Windows.h> does not pull in under LEAN_AND_MEAN
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>        // _dyld_image_count, _dyld_get_image_header, _dyld_get_image_name
#include <mach-o/dyld_images.h> // dyld_all_image_infos
#include <mach-o/loader.h>      // the 64-bit header and its load commands
#include <mach/mach.h>          // task_info(TASK_DYLD_INFO), which names dyld itself
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
#elif defined(__APPLE__)
constexpr bool has_module_enumeration = true;

/// A mapped image's `__TEXT` size and its LC_UUID, read from its load commands in memory.
///
/// The header is `__TEXT`'s first byte, so `__TEXT` alone spans every address an image's code can have.
/// Its segments may lie far apart in the shared cache, which is why the span is not the image's whole extent.
/// The identity is the UUID as `dwarfdump --uuid` prints it, which is what a dSYM is matched on.
struct mach_image_info
{
    u64 text_size = 0;
    cc::string identity;
};

[[nodiscard]] mach_image_info read_mach_image(mach_header const* header)
{
    auto info = mach_image_info();
    if (header == nullptr || header->magic != MH_MAGIC_64)
        return info;

    auto const* cursor = reinterpret_cast<byte const*>(header) + sizeof(mach_header_64);
    for (auto i = u32(0); i < header->ncmds; ++i)
    {
        auto const* const command = reinterpret_cast<load_command const*>(cursor);
        if (command->cmd == LC_SEGMENT_64)
        {
            auto const* const segment = reinterpret_cast<segment_command_64 const*>(command);
            // not null-terminated when it fills all 16 bytes
            auto length = isize(0);
            while (length < isize(sizeof(segment->segname)) && segment->segname[length] != '\0')
                ++length;
            if (cc::string_view(segment->segname, length) == SEG_TEXT)
                info.text_size = segment->vmsize;
        }
        else if (command->cmd == LC_UUID)
        {
            auto const* const u = reinterpret_cast<uuid_command const*>(command)->uuid;
            info.identity = cc::format(
                "{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        }
        cursor += command->cmdsize;
    }
    return info;
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
    // A process snapshot, which is a system call that walks every loaded module.
    CC_RECORD_SCOPE("cc.modules.enumerate");

    cc::vector<cc::loaded_module> out;

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // The snapshot fails with ERROR_BAD_LENGTH while this process is loading or unloading a module, and the documented
    // remedy is to ask again.
    // It is not rare: about one call in thirty under steady module churn, and an unretried one hands back an EMPTY
    // table, which a crash dump then carries instead of the one that would have symbolized it.
    auto* snap = INVALID_HANDLE_VALUE;
    for (auto attempt = 0; attempt < 64; ++attempt)
    {
        snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE)
            break;
        if (::GetLastError() != ERROR_BAD_LENGTH)
            return out;
    }
    if (snap == INVALID_HANDLE_VALUE)
        return out;

    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (::Module32First(snap, &entry) != 0)
        do
        {
            auto const base = reinterpret_cast<u64>(entry.modBaseAddr);

            // Pinned while its headers are read: the snapshot is already stale, and a module another thread unloaded
            // since has no headers left to read.
            // One that is gone, or replaced by another at that base, is no longer loaded and so is left out.
            HMODULE pinned = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(base), &pinned)
                == 0)
                continue;
            if (reinterpret_cast<u64>(pinned) == base)
                out.push_back({
                    .base = base,
                    .size = u64(entry.modBaseSize),
                    .path = cc::string(entry.szExePath),
                    .identity = pe_identity(base),
                });
            ::FreeLibrary(pinned);
        } while (::Module32Next(snap, &entry) != 0);

    ::CloseHandle(snap);
#elif defined(__APPLE__)
    // dyld's list may shrink while it is read, when another thread unloads an image: a slot that went away reads null.
    auto const count = _dyld_image_count();
    out.reserve(isize(count));
    for (auto i = u32(0); i < count; ++i)
    {
        auto const* const header = _dyld_get_image_header(i);
        auto const* const name = _dyld_get_image_name(i);
        if (header == nullptr || name == nullptr)
            continue;
        auto info = read_mach_image(header);
        if (info.text_size == 0)
            continue;
        out.push_back({
            .base = reinterpret_cast<u64>(header),
            .size = info.text_size,
            .path = cc::string(name),
            .identity = cc::move(info.identity),
        });
    }

    // dyld is in no list of its own, yet every main thread's outermost frame is in it
    auto dyld_info = task_dyld_info_data_t{};
    auto dyld_info_count = mach_msg_type_number_t(TASK_DYLD_INFO_COUNT);
    if (task_info(mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyld_info), &dyld_info_count)
        == KERN_SUCCESS)
    {
        auto const* const all = reinterpret_cast<dyld_all_image_infos const*>(uintptr_t(dyld_info.all_image_info_addr));
        auto const* const header = all != nullptr ? all->dyldImageLoadAddress : nullptr;
        if (auto info = read_mach_image(header); info.text_size != 0)
            out.push_back({
                .base = reinterpret_cast<u64>(header),
                .size = info.text_size,
                .path = cc::string(all->dyldPath != nullptr ? all->dyldPath : "/usr/lib/dyld"),
                .identity = cc::move(info.identity),
            });
    }
#endif

    return out;
}
