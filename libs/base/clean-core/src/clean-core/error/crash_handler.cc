#include "crash_handler.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/impl/posix_thread_stacks.hh>
#include <clean-core/platform/stacktrace.hh>
#include <clean-core/platform/symbolize.hh> // cc::impl::with_dbghelp_if_free, so the walk never waits on a suspended thread's lock
#include <clean-core/record/thread_scopes.hh>

#include <csignal>
#include <cstdio>
#include <cstdlib>


#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>
#include <dbghelp.h>  // StackWalk64 / SymFromAddr — the only way to walk a thread that is not this one
#include <tlhelp32.h> // CreateToolhelp32Snapshot — and the only way to enumerate this process's threads
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
void report_stacks(char const* banner, char const* reason) noexcept
{
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
}

void report_crash(char const* reason) noexcept
{
    report_stacks("\n===================== fatal crash =====================\n", reason);
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

void report_overflow() noexcept
{
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
        SetEvent(g_overflow_request);
        WaitForSingleObject(g_overflow_done, INFINITE);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    report_crash(seh_reason(code));
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
    report_crash("abort() / std::terminate");
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
    report_crash(signal_reason(sig));
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

    // Reserved now rather than when something goes wrong, which is the whole point: the collector and its handler
    // both run in a process that is already in trouble, and neither may allocate.
    cc::impl::install_posix_thread_stack_reporter();
#endif
}

void cc::report_all_thread_stacks(char const* reason) noexcept
{
#if CC_CRASH_HANDLER_SANITIZED
    (void)reason;
#else
    report_stacks("\n===================== all thread stacks =====================\n",
                  reason == nullptr ? "not given" : reason);
#endif
}

void cc::add_crash_context_hook(cc::crash_context_hook hook)
{
    if (hook == nullptr || g_hook_count >= k_max_hooks)
        return;
    g_hooks[g_hook_count++] = hook;
}
