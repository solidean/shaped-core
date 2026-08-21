#include "symbolize.hh"

#include <clean-core/string/format.hh>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <clean-core/platform/win32_sanitized.hh>
#include <dbghelp.h> // SymFromAddr / SymGetLineFromAddr64, the only route to a name on Windows
#endif

using namespace cc::primitive_defines;

namespace
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
constexpr bool has_symbolization = true;

/// The file name of a path, so a MODULE reads as `app.exe` rather than wherever it was installed from.
/// Source paths deliberately keep theirs — see resolve.
[[nodiscard]] cc::string_view file_name_of(char const* path)
{
    cc::string_view const s = path != nullptr ? path : "";
    for (auto i = s.size(); i > 0; --i)
        if (s[i - 1] == '\\' || s[i - 1] == '/')
            return s.subview(i);
    return s;
}

/// Brings the process's symbol handler up, exactly once.
///
/// **Never torn down.** The DbgHelp session is process-global and shared with the crash handler, so a symbolizer that
/// cleaned up after itself would take the crash reporter's symbols with it.
void ensure_symbol_handler()
{
    static bool const once = []
    {
        // DEFERRED_LOADS so standing this up does not read every PDB the process has; LOAD_LINES because a file and a
        // line are most of the value; UNDNAME so a name reads as C++ rather than as a mangling.
        ::SymSetOptions(::SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

        // The return is deliberately ignored: the crash handler may already have initialized the session, in which
        // case this fails and the session is exactly as usable.
        ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
        return true;
    }();
    (void)once;
}
#else
constexpr bool has_symbolization = false;
#endif
} // namespace

cc::string cc::symbol_info::to_string() const
{
    if (has_function() && has_line())
        return cc::format("{} at {}:{}", function, file, line);
    if (has_function())
        return displacement > 0 ? cc::format("{}+0x{:x}", function, displacement) : function;
    if (!module.empty())
        return cc::format("{}+0x{:x}", module, module_offset);
    return "<unknown>";
}

bool cc::symbolizer::is_available()
{
    return has_symbolization;
}

cc::symbolizer::symbolizer()
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    ensure_symbol_handler();
#endif
}

cc::symbol_info const& cc::symbolizer::resolve(void const* address)
{
    auto const key = reinterpret_cast<u64>(address);
    if (auto* const hit = _cache.get_ptr(key); hit != nullptr)
        return *hit;

    auto& out = _cache[key];

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    auto const process = ::GetCurrentProcess();

    // The name is variable-length and lives past the end of the struct, which is why this is a byte buffer rather
    // than a SYMBOL_INFO.
    alignas(SYMBOL_INFO) byte buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (::SymFromAddr(process, key, &displacement, symbol) != 0)
    {
        out.function = cc::string_view(symbol->Name, isize(symbol->NameLen));
        out.displacement = u64(displacement);
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (::SymGetLineFromAddr64(process, key, &line_displacement, &line) != 0)
    {
        // The whole path, not just the file name: a reader following a profile wants to open the file, and two
        // `renderer.cc` in different directories are otherwise indistinguishable.
        out.file = line.FileName != nullptr ? cc::string_view(line.FileName) : cc::string_view();
        out.line = i32(line.LineNumber);
    }

    // The module and the offset into it are what still locate a frame in a disassembly when there are no symbols at
    // all, which is exactly the case an optimized third-party module produces.
    if (auto const base = ::SymGetModuleBase64(process, key); base != 0)
    {
        IMAGEHLP_MODULE64 info = {};
        info.SizeOfStruct = sizeof(info);
        if (::SymGetModuleInfo64(process, base, &info) != 0)
            out.module = file_name_of(info.ImageName);

        out.module_offset = key - u64(base);
    }
#endif

    return out;
}
