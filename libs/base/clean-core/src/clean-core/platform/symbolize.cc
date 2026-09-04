#include "symbolize.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <clean-core/platform/win32_sanitized.hh>
#include <dbghelp.h> // SymFromAddr / SymGetLineFromAddr64, the only route to a name on Windows
#endif

using namespace cc::primitive_defines;

namespace
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
constexpr bool has_symbolization = true;

/// Serializes every DbgHelp call this process makes.
///
/// DbgHelp is documented single-threaded, and its state is process-wide rather than per-session: two threads
/// resolving through DIFFERENT session handles still land in the same library state, so a per-symbolizer lock would
/// not do and neither would one per session.
/// The failure mode is why this is not left to callers -- a race does not lose a name, it corrupts the heap, and the
/// crash then lands in an unrelated allocation far from here.
///
/// Nothing under it may symbolize, since re-entering here on the same thread would deadlock: a plain log is fine,
/// a stacktrace capture that resolves its frames is not.
///
/// The guarded value is a tag rather than state of ours -- DbgHelp owns the real state, and holding this is the right
/// to call into it.
struct dbghelp_access
{
};

cc::mutex<dbghelp_access>& dbghelp_lock()
{
    static cc::mutex<dbghelp_access> m;
    return m;
}

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

/// Whether DbgHelp can open this module's image without a network round trip.
///
/// It loads an image lazily, inside the first SymFromAddr, so an unreachable path does not fail at load time —
/// it blocks at resolve time, once per address, for however long the network takes to give up.
/// A mapped drive whose server is gone is the bad case: tens of seconds each, where a missing local file is
/// instant.
/// A foreign table routinely carries paths from a machine that is not this one, so this is the ordinary case
/// rather than a corner.
[[nodiscard]] bool is_locally_reachable(cc::string_view path)
{
    auto const is_sep = [](char c) { return c == '\\' || c == '/'; };

    // A `\\?\` or `\\.\` prefix turns off path parsing; what follows is `UNC\server\share` or an ordinary drive path.
    // Stripping it first is what keeps a local `\\?\C:\...` off the UNC branch its two leading separators would
    // otherwise take.
    auto const has_device_prefix = path.size() >= 4 && is_sep(path[0]) && is_sep(path[1])
                                && (path[2] == '?' || path[2] == '.') && is_sep(path[3]);
    if (has_device_prefix)
    {
        path = path.subview(4);

        // The one device path that still names another machine.
        auto const is_unc = path.size() >= 4 && (path[0] == 'U' || path[0] == 'u') && (path[1] == 'N' || path[1] == 'n')
                         && (path[2] == 'C' || path[2] == 'c') && is_sep(path[3]);
        if (is_unc)
            return false;
    }
    // A UNC path names another machine outright.
    else if (path.size() >= 2 && is_sep(path[0]) && is_sep(path[1]))
        return false;

    // Not rooted at a drive letter: a relative or device path, which resolves against this process and not a server.
    if (path.size() < 3 || path[1] != ':')
        return true;

    // A mount-table lookup, and deliberately not a file open — the point is to answer without touching the network.
    char const root[4] = {path[0], ':', '\\', '\0'};
    auto const type = ::GetDriveTypeA(root);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK || type == DRIVE_CDROM;
}

/// Brings the process's symbol handler up, exactly once.
///
/// **Never torn down.** The DbgHelp session is process-global and shared with the crash handler, so a symbolizer that
/// cleaned up after itself would take the crash reporter's symbols with it.
void ensure_symbol_handler()
{
    static bool const once = []
    {
        auto const guard = dbghelp_lock().lock_scoped();

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

cc::symbolizer::symbolizer(cc::span<cc::loaded_module const> modules, cc::symbolize_options opts)
{
    // Opens a DbgHelp session and loads a PDB per module, which is tens of milliseconds against a real symbol server.
    CC_RECORD_SCOPE("cc.symbolize.open");

    _modules.push_back_range(modules);

#if !defined(_WIN32) || defined(__EMSCRIPTEN__)
    (void)opts; // nothing to configure where there is no debug-info session to open
#endif

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (_modules.empty())
    {
        ensure_symbol_handler();
        return;
    }

    // A session of its own, keyed by a handle that is not a real process.
    //
    // DbgHelp keys everything on that handle and never dereferences it, which is what lets a foreign table be loaded
    // at ITS bases without disturbing the process session the crash handler shares.
    // A unique value per symbolizer, so two of them can be open at once.
    static cc::atomic<u64> next_session = 1;
    _session = reinterpret_cast<void*>(u64(0xCC5E5510u) << 32 | next_session.fetch_add(1, cc::memory_order_relaxed));

    // Held across the whole open, so a concurrent resolve cannot land between SymInitialize and the modules it needs.
    auto const guard = dbghelp_lock().lock_scoped();

    ::SymSetOptions(::SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (::SymInitialize(HANDLE(_session), nullptr, FALSE) == 0)
    {
        _session = nullptr;
        return;
    }

    for (auto& m : _modules)
    {
        // A module DbgHelp would have to reach across the network for is not registered at all, unless the caller
        // says the share answers.
        // Registering it buys symbols only when the remote machine is there, and costs an unbounded wait when it is
        // not — while resolve() names the frame from _modules either way, before it ever asks DbgHelp.
        if (!opts.load_remote_images && !is_locally_reachable(m.path))
        {
            // Says why these frames come back as `module+offset` when a reader expected names, which is otherwise
            // indistinguishable from the debug info simply being absent.
            CC_LOG_DEBUG("symbolize: {} not registered, its path is not locally reachable ({})", m.name(), m.path);
            continue;
        }

        // At the base the RECORDING used, not wherever this process would have put it.
        // A module whose file is missing simply fails to load, and resolve still reports its name and the offset from
        // the table itself.
        ::SymLoadModuleEx(HANDLE(_session), nullptr, m.path.c_str_materialize(), nullptr, DWORD64(m.base),
                          DWORD(m.size), nullptr, 0);
    }
#endif
}

cc::symbolizer::~symbolizer()
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Only a session this symbolizer opened.
    // The process session belongs to the whole process and is deliberately never cleaned up.
    if (_session != nullptr)
    {
        auto const guard = dbghelp_lock().lock_scoped();
        ::SymCleanup(HANDLE(_session));
    }
#endif
}

cc::symbol_info const& cc::symbolizer::resolve(void const* address)
{
    auto const key = reinterpret_cast<u64>(address);
    if (auto* const hit = _cache.get_ptr(key); hit != nullptr)
        return *hit;

    // Misses only: a hit is a map lookup, and counting those would measure how often a caller asks rather than what
    // symbolization cost.
    CC_RECORD_ACCUM("cc.symbolize.resolved", cc::rec::unit_count, 1);

    auto& out = _cache[key];

    // From the recorded table, so a module still names the frame even when its binary is nowhere to be found.
    // Filled first on purpose: whatever the debug info manages afterwards only ever improves on it.
    for (auto const& m : _modules)
        if (m.contains(key))
        {
            out.module = m.name();
            out.module_offset = key - m.base;
            break;
        }

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    auto const process = _session != nullptr ? HANDLE(_session) : ::GetCurrentProcess();
    if (is_foreign() && _session == nullptr)
        return out; // the session could not be opened, so the table is all there is

    // Guards the platform, not this object: the cache above is still one symbolizer's own, so a single instance
    // remains single-threaded while two instances on two threads are now safe.
    auto const guard = dbghelp_lock().lock_scoped();

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
    // Skipped for a foreign table, which already answered this and answered it from the recording.
    if (!is_foreign())
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
