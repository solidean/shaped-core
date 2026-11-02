#include "crash_handler.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/impl/posix_thread_stacks.hh>
#include <clean-core/platform/stacktrace.hh>
#include <clean-core/platform/symbolize.hh> // cc::impl::with_dbghelp_if_free, so the walk never waits on a suspended thread's lock
#include <clean-core/record/thread_scopes.hh>
#include <clean-core/thread/atomic.hh>
#include <fcntl.h>
#include <sys/stat.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>
#include <dbghelp.h>  // StackWalk64 / SymFromAddr — the only way to walk a thread that is not this one
#include <direct.h>   // _mkdir
#include <io.h>       // _open / _dup2, to point stderr at a report's file
#include <tlhelp32.h> // CreateToolhelp32Snapshot — and the only way to enumerate this process's threads
#else
#include <unistd.h>
#endif

namespace
{
// Context hooks are stored in a fixed array so the crash path never allocates.
constexpr int k_max_hooks = 16;
cc::crash_context_hook g_hooks[k_max_hooks] = {};
int g_hook_count = 0;
bool g_installed = false;

// Everything below is the actual fault-reporting machinery, compiled out under sanitizers (see CC_CRASH_HANDLER_SANITIZED) so the runtime's own handlers stay in place.
#if !CC_CRASH_HANDLER_SANITIZED

// =========================================================================================================
// report files
// =========================================================================================================

// A report's path is formatted at install up to the time, so the fault path appends digits and allocates nothing.
constexpr int k_max_report_path = 1024;
char g_report_prefix[k_max_report_path] = {}; // "<directory>/<name>-", or empty when reports stay on stderr
int g_report_prefix_length = 0;
bool g_report_minidump = false;
int g_report_count = 0; // reports this process kept: a second in the same second gets a name of its own
char g_report_stem[k_max_report_path] = {}; // the last report's path without its extension
int g_report_stem_length = 0;

void append_text(char* out, int& length, char const* text) noexcept
{
    for (; *text != '\0' && length < k_max_report_path - 8; ++text) // room left for an extension
        out[length++] = *text;
    out[length] = '\0';
}

void append_number(char* out, int& length, unsigned long long value, int min_digits) noexcept
{
    char digits[24];
    auto n = 0;
    do
    {
        digits[n++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0 || n < min_digits);
    while (n > 0 && length < k_max_report_path - 8)
        out[length++] = digits[--n];
    out[length] = '\0';
}

#ifdef CC_OS_WINDOWS
int sys_open_report(char const* path) noexcept
{
    auto fd = -1;
    return _sopen_s(&fd, path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 ? fd
                                                                                                                 : -1;
}
int sys_dup(int fd) noexcept
{
    return _dup(fd);
}
int sys_dup2(int from, int to) noexcept
{
    return _dup2(from, to);
}
int sys_close(int fd) noexcept
{
    return _close(fd);
}
long sys_rewind(int fd) noexcept
{
    return _lseek(fd, 0, SEEK_SET);
}
int sys_read(int fd, char* buffer, int size) noexcept
{
    return _read(fd, buffer, unsigned(size));
}
int sys_write(int fd, char const* buffer, int size) noexcept
{
    return _write(fd, buffer, unsigned(size));
}
unsigned long long process_id() noexcept
{
    return GetCurrentProcessId();
}
int make_directory(char const* path) noexcept
{
    return _mkdir(path);
}
#else
int sys_open_report(char const* path) noexcept
{
    return open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
}
int sys_dup(int fd) noexcept
{
    return dup(fd);
}
int sys_dup2(int from, int to) noexcept
{
    return dup2(from, to);
}
int sys_close(int fd) noexcept
{
    return close(fd);
}
long sys_rewind(int fd) noexcept
{
    return long(lseek(fd, 0, SEEK_SET));
}
int sys_read(int fd, char* buffer, int size) noexcept
{
    return int(read(fd, buffer, size_t(size)));
}
int sys_write(int fd, char const* buffer, int size) noexcept
{
    return int(write(fd, buffer, size_t(size)));
}
unsigned long long process_id() noexcept
{
    return static_cast<unsigned long long>(getpid());
}
int make_directory(char const* path) noexcept
{
    return mkdir(path, 0755);
}
#endif

/// Creates `path` and every missing parent; an existing directory is fine.
void make_directories(char* path, int length) noexcept
{
    for (auto i = 1; i <= length; ++i)
    {
        if (i < length && path[i] != '/' && path[i] != '\\')
            continue;
        // a drive's root, `C:`, is not a directory to make
        if (i == 2 && path[1] == ':')
            continue;
        auto const kept = path[i];
        path[i] = '\0';
        (void)make_directory(path);
        path[i] = kept;
    }
}

/// The next report's path without its extension, in g_report_stem: the prefix, the local time, the process, and a count past the first.
void format_report_stem() noexcept
{
    auto length = 0;
    append_text(g_report_stem, length, g_report_prefix);
    auto const now = std::time(nullptr);
    std::tm local = {};
#ifdef CC_OS_WINDOWS
    (void)localtime_s(&local, &now);
#else
    (void)localtime_r(&now, &local);
#endif
    append_number(g_report_stem, length, static_cast<unsigned long long>(local.tm_year) + 1900, 4);
    append_number(g_report_stem, length, static_cast<unsigned long long>(local.tm_mon) + 1, 2);
    append_number(g_report_stem, length, static_cast<unsigned long long>(local.tm_mday), 2);
    append_text(g_report_stem, length, "-");
    append_number(g_report_stem, length, static_cast<unsigned long long>(local.tm_hour), 2);
    append_number(g_report_stem, length, static_cast<unsigned long long>(local.tm_min), 2);
    append_number(g_report_stem, length, static_cast<unsigned long long>(local.tm_sec), 2);
    append_text(g_report_stem, length, "-");
    append_number(g_report_stem, length, process_id(), 1);
    if (g_report_count > 0)
    {
        append_text(g_report_stem, length, "-");
        append_number(g_report_stem, length, static_cast<unsigned long long>(g_report_count), 1);
    }
    ++g_report_count;
    g_report_stem_length = length;
}

/// The last report's path with `extension` added, in `out`.
void report_path(char* out, char const* extension) noexcept
{
    auto length = 0;
    append_text(out, length, g_report_stem);
    for (; *extension != '\0'; ++extension)
        out[length++] = *extension;
    out[length] = '\0';
}

/// Where stderr went while a report was written to its file: the file, the stream it replaced, and whether this report holds the report lock.
struct report_capture
{
    int file = -1;
    int saved = -1;
    bool owned = false;
};

// The thread writing a report to a file, as a token, or 0: stderr has one target, so two reports at once would write into each other's files.
// A watchdog's hang report and the fault a driver induces to end the same hang are exactly two at once.
cc::atomic<uintptr_t> g_report_owner = 0;

uintptr_t this_thread_token() noexcept
{
    thread_local char marker = 0;
    return reinterpret_cast<uintptr_t>(&marker);
}

void sleep_briefly() noexcept
{
#ifdef CC_OS_WINDOWS
    Sleep(10);
#else
    usleep(10000);
#endif
}

/// Points stderr at a new report file, so whatever the report writes, hooks included, lands there; a no-op without report files.
/// Waits up to 5 s for a report another thread is writing, then writes to stderr alone; a fault inside a report on this thread does so at once.
report_capture begin_report_file() noexcept
{
    if (g_report_prefix_length == 0)
        return {};
    auto const self = this_thread_token();
    if (g_report_owner.load(cc::memory_order_acquire) == self)
        return {};
    for (auto waited = 0;; ++waited)
    {
        auto expected = uintptr_t(0);
        if (g_report_owner.compare_exchange_strong(expected, self, cc::memory_order_acq_rel))
            break;
        if (waited == 500)
            return {};
        sleep_briefly();
    }
    format_report_stem();
    char path[k_max_report_path];
    report_path(path, ".txt");
    std::fflush(stderr);
    auto const file = sys_open_report(path);
    auto const saved = file < 0 ? -1 : sys_dup(2);
    if (file < 0 || saved < 0 || sys_dup2(file, 2) < 0)
    {
        if (saved >= 0)
            (void)sys_close(saved);
        if (file >= 0)
            (void)sys_close(file);
        return {.owned = true};
    }
    return {.file = file, .saved = saved, .owned = true};
}

/// Lets the next report write its file; after the minidump, which names itself after this report.
void release_report(report_capture const& capture) noexcept
{
    if (capture.owned)
        g_report_owner.store(0, cc::memory_order_release);
}

/// Gives stderr back, copies the report to it, and says where the file is.
void end_report_file(report_capture const& capture) noexcept
{
    if (capture.file < 0)
        return;
    std::fflush(stderr);
    (void)sys_dup2(capture.saved, 2);
    (void)sys_close(capture.saved);
    if (sys_rewind(capture.file) == 0)
    {
        char buffer[4096];
        for (auto n = sys_read(capture.file, buffer, int(sizeof(buffer))); n > 0;
             n = sys_read(capture.file, buffer, int(sizeof(buffer))))
            (void)sys_write(2, buffer, n);
    }
    (void)sys_close(capture.file);
    std::fprintf(stderr, "crash report kept: %s.txt\n", g_report_stem);
    std::fflush(stderr);
}

#ifdef CC_OS_WINDOWS

// Every OTHER thread's stack, walked one thread at a time.
//
// The faulting thread reports itself through std::stacktrace; this covers the rest, which is what a deadlock or a hung test actually needs.
// There, the interesting stack is never the one that noticed.
// std::stacktrace can only capture the calling thread, so this drops to DbgHelp: snapshot the thread list, then per thread suspend / GetThreadContext / StackWalk64 / resume.
//
// Deliberately one at a time.
// Suspending every thread up front and then symbolizing would deadlock the moment a suspended thread held the heap or loader lock that DbgHelp needs.
// Holding at most one suspension at a time keeps that window as small as it can be.
// It is not zero — this is a diagnostic on an already-dying process, as the header says — so every step is best-effort and bounded.
constexpr int k_max_threads = 64; // a runaway thread count must not turn a crash report into a flood
constexpr int k_max_frames = 64;

/// The file name of a path, so a frame's module reads as `foo.exe` rather than its whole install path.
char const* file_name_of(char const* path) noexcept
{
    char const* out = path;
    for (char const* p = path; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/')
            out = p + 1;
    return out;
}

void report_frame(DWORD64 address, SYMBOL_INFO* sym, IMAGEHLP_LINE64* line) noexcept
{
    std::fprintf(stderr, "    0x%016llx", static_cast<unsigned long long>(address));

    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, sym))
    {
        std::fprintf(stderr, " %s", sym->Name);
    }
    else
    {
        // No symbol — a release build without a PDB, or a module we have no symbols for.
        // `module+offset` still finds the frame in a disassembly, which is the difference between a usable report and a column of bare addresses.
        // That is exactly the case a release-build hang produces.
        IMAGEHLP_MODULE64 module;
        cc::memset(&module, 0, sizeof(module));
        module.SizeOfStruct = sizeof(module);
        auto const base = SymGetModuleBase64(GetCurrentProcess(), address);
        if (base != 0 && SymGetModuleInfo64(GetCurrentProcess(), base, &module))
            std::fprintf(stderr, " %s+0x%llx", file_name_of(module.ImageName),
                         static_cast<unsigned long long>(address - base));
    }

    DWORD line_displacement = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &line_displacement, line))
        std::fprintf(stderr, " at %s:%lu", line->FileName, line->LineNumber);

    std::fputc('\n', stderr);
}

/// Walks `thread` from `ctx`, which the walk consumes, and reports its first `head` frames.
/// With `tail` > 0 it walks on to the bottom, up to a bound, and also reports the last `tail` frames and the depth: an overflowed stack's recursion shows at the top, its entry at the bottom.
void walk_frames(HANDLE thread, CONTEXT& ctx, int head, int tail) noexcept
{
    STACKFRAME64 frame;
    cc::memset(&frame, 0, sizeof(frame));
#if defined(_M_ARM64) || defined(__aarch64__)
    DWORD const machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#else
    DWORD const machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    // SYMBOL_INFO is a header plus an inline name buffer, so it is allocated as raw bytes.
    alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* const sym = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    IMAGEHLP_LINE64 line;
    cc::memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    // the last `tail` frames, in a ring, since the depth is only known at the bottom
    constexpr int k_max_tail = 16;
    constexpr int k_max_depth = 1 << 20;
    DWORD64 ring[k_max_tail] = {};
    tail = tail < k_max_tail ? tail : k_max_tail;
    auto const bound = tail > 0 ? k_max_depth : head;
    int depth = 0;
    for (; depth < bound; ++depth)
    {
        if (!StackWalk64(machine, GetCurrentProcess(), thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;
        if (depth < head)
            report_frame(frame.AddrPC.Offset, sym, &line);
        else
            ring[(depth - head) % k_max_tail] = frame.AddrPC.Offset;
    }
    if (tail == 0 || depth <= head)
        return;

    auto const beyond = depth - head; // frames past the head, of which the ring holds the last min(beyond, k_max_tail)
    auto const shown = beyond < tail ? beyond : tail;
    std::fprintf(stderr, "    ... %d frames ...\n", beyond - shown);
    for (int i = beyond - shown; i < beyond; ++i)
        report_frame(ring[i % k_max_tail], sym, &line);
    std::fprintf(stderr, "    (%s%d frames in all)\n", depth == bound ? "at least " : "", depth);
}

void walk_thread(HANDLE thread) noexcept
{
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
    {
        std::fputs("    <could not suspend this thread>\n", stderr);
        return;
    }

    CONTEXT ctx;
    cc::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx))
    {
        std::fputs("    <could not read this thread's context>\n", stderr);
        ResumeThread(thread);
        return;
    }
    walk_frames(thread, ctx, k_max_frames, 0);
    ResumeThread(thread);
}

// The thread that reports a stack overflow, parked from install on: the overflowed thread has no stack left to report on.
DWORD g_reporter_id = 0;

/// Walks every thread of this process but this one and `skip`, which the caller reports itself; inside the DbgHelp guard, symbols initialized.
void walk_other_threads(HANDLE snapshot, DWORD skip) noexcept
{
    auto const self = GetCurrentThreadId();
    THREADENTRY32 entry;
    cc::memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);

    bool any = false;
    int reported = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self
            || entry.th32ThreadID == skip || entry.th32ThreadID == g_reporter_id)
            continue;
        if (reported++ >= k_max_threads)
        {
            std::fprintf(stderr, "\n<more threads follow; stopped after %d>\n", k_max_threads);
            break;
        }

        if (!any)
        {
            std::fputs("\nother threads:\n", stderr);
            any = true;
        }
        std::fprintf(stderr, "  thread %lu:\n", entry.th32ThreadID);

        auto const thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                       entry.th32ThreadID);
        if (thread == nullptr)
        {
            std::fputs("    <could not open this thread>\n", stderr);
            continue;
        }
        walk_thread(thread);
        CloseHandle(thread);
    }
    if (!any)
        std::fputs("\nother threads: none\n", stderr);
}

/// Runs `walk` inside the DbgHelp guard with symbols initialized; false if another thread holds the guard.
template <class F>
bool with_symbols(F&& walk) noexcept
{
    // Everything below calls DbgHelp, on threads this loop SUSPENDS.
    //
    // cc::symbolizer serializes its own DbgHelp calls behind a process-global lock, and this must not simply take it:
    // a thread frozen while owning it never releases, so waiting would hang the crash report outright.
    // So it asks.
    // Holding the lock for the walk also keeps any other live thread out of DbgHelp meanwhile.
    // If it is already held, someone is mid-symbolization and walking anyway risks the hang this exists to avoid, so
    // the report says so and stops.
    //
    // A narrowing rather than a guarantee: DbgHelp entered by anything that is not cc::symbolizer is invisible here.
    return cc::impl::with_dbghelp_if_free(
        [&]
        {
            // Inside the guard, because these are DbgHelp calls like any other.
            // Symbols are initialized here rather than at install time: the handler must stay free of setup cost for
            // the overwhelmingly common case where the process never crashes at all.
            SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            walk();
            // Also inside: this tears down the session cc::symbolizer shares, so it must not race one.
            SymCleanup(GetCurrentProcess());
        });
}

void report_other_thread_stacks() noexcept
{
    auto const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        std::fputs("\n<could not enumerate threads>\n", stderr);
        return;
    }
    if (!with_symbols([&] { walk_other_threads(snapshot, 0); }))
        std::fputs("\nother threads: <not walked; another thread is inside the symbolizer>\n", stderr);
    CloseHandle(snapshot);
}

/// Writes a minidump beside the last report, `.dmp`, when report files ask for one; `thread` and `info` say where the fault was, if there was one.
/// Every thread's stack and the memory its pointers reach: locals and the objects they point at, without the whole heap.
void write_minidump(DWORD thread, EXCEPTION_POINTERS* info) noexcept
{
    if (!g_report_minidump || g_report_stem_length == 0)
        return;
    char path[k_max_report_path];
    report_path(path, ".dmp");
    auto const file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        std::fprintf(stderr, "minidump not written: cannot create %s\n", path);
        return;
    }
    auto exception
        = MINIDUMP_EXCEPTION_INFORMATION{.ThreadId = thread, .ExceptionPointers = info, .ClientPointers = FALSE};
    auto const type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo
                                                 | MiniDumpWithUnloadedModules);
    auto written = false;
    // DbgHelp again, so behind its guard: a thread frozen inside it would hang the dump
    auto const ran = cc::impl::with_dbghelp_if_free(
        [&]
        {
            written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                        info != nullptr ? &exception : nullptr, nullptr, nullptr)
                   != FALSE;
        });
    CloseHandle(file);
    if (written)
        std::fprintf(stderr, "minidump kept: %s\n", path);
    else
        std::fprintf(stderr, "minidump not written: %s\n",
                     ran ? "MiniDumpWriteDump failed" : "another thread is inside DbgHelp");
    std::fflush(stderr);
}

#else

// Walking a thread that is not the calling one has no portable equivalent here: backtrace() captures only the caller.
// Reaching the others means asking each to report ITSELF, through a signal — see impl/posix_thread_stacks.hh, which
// does that on Linux and nothing anywhere else yet.
// Where it is unavailable the faulting thread's stack is still reported, and a core dump has the rest.
void report_other_thread_stacks() noexcept
{
    std::fputs("\nother threads:\n", stderr);
    if (!cc::impl::report_posix_thread_stacks())
        std::fputs("  <not reached on this platform; the core dump has them>\n", stderr);
}

#endif

void run_hooks() noexcept
{
    for (int i = 0; i < g_hook_count; ++i)
        if (g_hooks[i] != nullptr)
            g_hooks[i]();
}

// Shared best-effort reporter: a banner, the reason, registered context, then a stacktrace of this thread and of
// every other one.
// Uses stdio (not std::cerr) to keep the crash path as small as possible.
// Returns the report's capture, still holding the report lock: the caller writes a minidump, then releases it.
[[nodiscard]] report_capture report_stacks(char const* banner, char const* reason) noexcept
{
    auto const capture = begin_report_file();
    std::fputs(banner, stderr);
    std::fputs("reason: ", stderr);
    std::fputs(reason, stderr);
    std::fputc('\n', stderr);

    run_hooks();

#if CC_HAS_STACKTRACE
    std::fputs("\nstacktrace (faulting thread):\n", stderr);
    auto const text = cc::to_string(cc::stacktrace::current());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
#else
    std::fputs("\n<stacktrace unavailable on this platform>\n", stderr);
#endif

    report_other_thread_stacks();

    // **A peer of the stacks above, not a fallback for them.**
    //
    // They answer different questions: a stack says where the program counter is, and the scope stack says which
    // logical task a thread is inside.
    // In a deadlock the second is usually what identifies the bug, and it is the one the machine stacks above cannot
    // reach at all off Windows -- or, on wasm, ever.
    cc::rec::report_thread_scopes(reason);

    std::fputs("=======================================================\n", stderr);
    std::fflush(stderr);
    end_report_file(capture);
    return capture;
}

[[nodiscard]] report_capture report_crash(char const* reason) noexcept
{
    return report_stacks("\n===================== fatal crash =====================\n", reason);
}

#ifdef CC_OS_WINDOWS

char const* seh_reason(unsigned long code) noexcept
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation (segfault)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW:
        return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "datatype misalignment";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "float divide by zero";
    case EXCEPTION_BREAKPOINT:
        return "breakpoint / __debugbreak (often std::terminate or a failed assert unwinding through noexcept)";
    default:
        return "unhandled structured exception";
    }
}

// A stack overflow is reported by the parked reporter thread: the filter runs on the overflowed stack, with a few KiB left, and reporting there overflows again.
// The overflowed thread only hands over its id and context, then waits; the reporter walks it from that context.
HANDLE g_overflow_request = nullptr;
HANDLE g_overflow_done = nullptr;
LONG g_overflow_claimed = 0;
DWORD g_overflow_thread = 0;
CONTEXT g_overflow_context;
EXCEPTION_POINTERS* g_overflow_info = nullptr;

void report_overflow() noexcept
{
    auto const capture = begin_report_file();
    std::fputs("\n===================== fatal crash =====================\n", stderr);
    std::fputs("reason: stack overflow\n", stderr);
    run_hooks();

    std::fprintf(stderr, "\nstacktrace (faulting thread %lu, walked by the crash reporter):\n", g_overflow_thread);
    auto const thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_overflow_thread);
    auto const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    auto const walked = with_symbols(
        [&]
        {
            if (thread != nullptr)
                walk_frames(thread, g_overflow_context, 48, 16);
            else
                std::fputs("    <could not open the faulting thread>\n", stderr);
            if (snapshot != INVALID_HANDLE_VALUE)
                walk_other_threads(snapshot, g_overflow_thread);
        });
    if (!walked)
        std::fputs("    <not walked; another thread is inside the symbolizer>\n", stderr);
    if (snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(snapshot);
    if (thread != nullptr)
        CloseHandle(thread);

    cc::rec::report_thread_scopes("stack overflow");
    std::fputs("=======================================================\n", stderr);
    std::fflush(stderr);
    end_report_file(capture);
    write_minidump(g_overflow_thread, g_overflow_info);
    release_report(capture);
}

DWORD WINAPI reporter_main(void*) noexcept
{
    WaitForSingleObject(g_overflow_request, INFINITE);
    report_overflow();
    SetEvent(g_overflow_done);
    return 0;
}

LONG WINAPI seh_filter(EXCEPTION_POINTERS* info) noexcept
{
    unsigned long const code
        = (info != nullptr && info->ExceptionRecord != nullptr) ? info->ExceptionRecord->ExceptionCode : 0;
    if (code == EXCEPTION_STACK_OVERFLOW && info->ContextRecord != nullptr && g_reporter_id != 0
        && GetCurrentThreadId() != g_reporter_id)
    {
        if (InterlockedExchange(&g_overflow_claimed, 1) != 0)
            Sleep(INFINITE); // another thread's overflow is being reported, and the process ends after it
        g_overflow_thread = GetCurrentThreadId();
        g_overflow_context = *info->ContextRecord;
        g_overflow_info = info;
        SetEvent(g_overflow_request);
        WaitForSingleObject(g_overflow_done, INFINITE);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    auto const capture = report_crash(seh_reason(code));
    write_minidump(GetCurrentThreadId(), info);
    release_report(capture);
    return EXCEPTION_EXECUTE_HANDLER; // let the process terminate
}

/// Starts the parked reporter thread, with a stack of its own for the report; without it an overflow is reported in place, as any fault.
void start_overflow_reporter() noexcept
{
    g_overflow_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_overflow_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_overflow_request == nullptr || g_overflow_done == nullptr)
        return;
    DWORD id = 0;
    auto const thread = CreateThread(nullptr, 1 << 20, reporter_main, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (thread == nullptr)
        return;
    CloseHandle(thread);
    g_reporter_id = id;
}

void abort_handler(int) noexcept
{
    auto const capture = report_crash("abort() / std::terminate");
    write_minidump(GetCurrentThreadId(), nullptr);
    release_report(capture);
    std::signal(SIGABRT, SIG_DFL);
    std::_Exit(3);
}

void install_platform_handlers() noexcept
{
    start_overflow_reporter();
    SetUnhandledExceptionFilter(seh_filter);
    std::signal(SIGABRT, abort_handler);
}

#else // POSIX

char const* signal_reason(int sig) noexcept
{
    switch (sig)
    {
    case SIGSEGV:
        return "segmentation fault";
    case SIGABRT:
        return "abort() / std::terminate";
    case SIGILL:
        return "illegal instruction";
    case SIGFPE:
        return "floating point exception";
#ifdef SIGBUS
    case SIGBUS:
        return "bus error";
#endif
    default:
        return "fatal signal";
    }
}

void signal_handler(int sig) noexcept
{
    release_report(report_crash(signal_reason(sig)));
    // Restore the default disposition and re-raise so the process exits with the
    // signal's normal status (and can still produce a core dump).
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void install_platform_handlers() noexcept
{
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGILL, signal_handler);
    std::signal(SIGFPE, signal_handler);
#ifdef SIGBUS
    std::signal(SIGBUS, signal_handler);
#endif
}

#endif

#endif // !CC_CRASH_HANDLER_SANITIZED
} // namespace

void cc::install_crash_handler()
{
    if (g_installed)
        return;
    g_installed = true;
#if !CC_CRASH_HANDLER_SANITIZED
    install_platform_handlers();

    // Reserved now rather than when something goes wrong, because the collector and its handler
    // both run in a process that is already in trouble, and neither may allocate.
    cc::impl::install_posix_thread_stack_reporter();
#endif
}

void cc::report_all_thread_stacks(char const* reason) noexcept
{
#if CC_CRASH_HANDLER_SANITIZED
    (void)reason;
#else
    auto const capture = report_stacks("\n===================== all thread stacks =====================\n",
                                       reason == nullptr ? "not given" : reason);
#ifdef CC_OS_WINDOWS
    write_minidump(GetCurrentThreadId(), nullptr);
#endif
    release_report(capture);
#endif
}

cc::string_view cc::last_crash_report_path()
{
#if CC_CRASH_HANDLER_SANITIZED
    return {};
#else
    return cc::string_view(g_report_stem, g_report_stem_length);
#endif
}

void cc::set_crash_report_files(cc::crash_report_files const& files)
{
#if CC_CRASH_HANDLER_SANITIZED
    (void)files;
#else
    g_report_minidump = files.minidump;
    if (files.directory.empty())
    {
        g_report_prefix_length = 0;
        g_report_prefix[0] = '\0';
        return;
    }
    auto length = 0;
    for (auto i = isize(0); i < files.directory.size() && length < k_max_report_path / 2; ++i)
        g_report_prefix[length++] = files.directory[i];
    g_report_prefix[length] = '\0';
    make_directories(g_report_prefix, length);
    g_report_prefix[length++] = '/';
    for (auto i = isize(0); i < files.name.size() && length < k_max_report_path * 3 / 4; ++i)
        g_report_prefix[length++] = files.name[i];
    g_report_prefix[length++] = '-';
    g_report_prefix[length] = '\0';
    g_report_prefix_length = length;
#endif
}

void cc::add_crash_context_hook(cc::crash_context_hook hook)
{
    if (hook == nullptr || g_hook_count >= k_max_hooks)
        return;
    g_hooks[g_hook_count++] = hook;
}
