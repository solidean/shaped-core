#include "crash_handler.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/platform/stacktrace.hh>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if CC_HAS_STACKTRACE
#include <string> // std::to_string(std::stacktrace) is the only renderer that produces frame text
#endif

#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>
#include <dbghelp.h>  // StackWalk64 / SymFromAddr — the only way to walk a thread that is not this one
#include <tlhelp32.h> // CreateToolhelp32Snapshot — and the only way to enumerate this process's threads
#endif

// Sanitizers (ASan/TSan/MSan) install their own fault handlers and print far richer diagnostics;
// overriding them would suppress those reports. Detect an active sanitizer and make installation a
// no-op there, leaving the runtime's handlers in place.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CC_CRASH_HANDLER_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define CC_CRASH_HANDLER_SANITIZED 1
#endif
#endif
#ifndef CC_CRASH_HANDLER_SANITIZED
#define CC_CRASH_HANDLER_SANITIZED 0
#endif

namespace
{
// Context hooks are stored in a fixed array so the crash path never allocates.
constexpr int k_max_hooks = 16;
cc::crash_context_hook g_hooks[k_max_hooks] = {};
int g_hook_count = 0;
bool g_installed = false;

// Everything below is the actual fault-reporting machinery, compiled out under sanitizers
// (see CC_CRASH_HANDLER_SANITIZED) so the runtime's own handlers stay in place.
#if !CC_CRASH_HANDLER_SANITIZED

#ifdef CC_OS_WINDOWS

// Every OTHER thread's stack, walked one thread at a time.
//
// The faulting thread reports itself through std::stacktrace; this covers the rest, which is what a
// deadlock or a hung test actually needs — there the interesting stack is never the one that noticed.
// std::stacktrace can only capture the calling thread, so this drops to DbgHelp: snapshot the thread
// list, then per thread suspend / GetThreadContext / StackWalk64 / resume.
//
// Deliberately one at a time. Suspending every thread up front and then symbolizing would deadlock the
// moment a suspended thread held the heap or loader lock that DbgHelp needs; holding at most one
// suspension at a time keeps that window as small as it can be. It is not zero — this is a diagnostic on
// an already-dying process, and the header says as much — so every step is best-effort and bounded.
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
        // No symbol — a release build without a PDB, or a module we have no symbols for. `module+offset`
        // is still enough to find the frame in a disassembly, and is the difference between a usable
        // report and a column of bare addresses, which is exactly the case a release-build hang produces.
        IMAGEHLP_MODULE64 module;
        std::memset(&module, 0, sizeof(module));
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

void walk_thread(HANDLE thread) noexcept
{
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
    {
        std::fputs("    <could not suspend this thread>\n", stderr);
        return;
    }

    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx))
    {
        std::fputs("    <could not read this thread's context>\n", stderr);
        ResumeThread(thread);
        return;
    }

    STACKFRAME64 frame;
    std::memset(&frame, 0, sizeof(frame));
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
    std::memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (int depth = 0; depth < k_max_frames; ++depth)
    {
        if (!StackWalk64(machine, GetCurrentProcess(), thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;
        report_frame(frame.AddrPC.Offset, sym, &line);
    }

    ResumeThread(thread);
}

void report_other_thread_stacks() noexcept
{
    auto const self = GetCurrentThreadId();
    auto const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        std::fputs("\n<could not enumerate threads>\n", stderr);
        return;
    }

    // Symbols are initialized here rather than at install time: the handler must stay free of setup cost
    // for the overwhelmingly common case where the process never crashes at all.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    THREADENTRY32 entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);

    bool any = false;
    int reported = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self)
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

    SymCleanup(GetCurrentProcess());
    CloseHandle(snapshot);
}

#else

// Walking a thread that is not the calling one has no portable equivalent here: backtrace() captures only
// the caller, and reaching the others means signalling each in turn — machinery this diagnostic does not
// justify. The faulting thread's stack is still reported, and a core dump has the rest.
void report_other_thread_stacks() noexcept
{
    std::fputs("\n<other threads' stacks are Windows-only; the core dump has them>\n", stderr);
}

#endif

// Shared best-effort reporter: fault description, registered context, then a stacktrace of this thread
// and of every other one.
// Uses stdio (not std::cerr) to keep the crash path as small as possible.
void report_crash(char const* reason) noexcept
{
    std::fputs("\n===================== fatal crash =====================\n", stderr);
    std::fputs("reason: ", stderr);
    std::fputs(reason, stderr);
    std::fputc('\n', stderr);

    for (int i = 0; i < g_hook_count; ++i)
        if (g_hooks[i] != nullptr)
            g_hooks[i]();

#if CC_HAS_STACKTRACE
    std::fputs("\nstacktrace (faulting thread):\n", stderr);
    auto const text = std::to_string(cc::stacktrace::current());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
#else
    std::fputs("\n<stacktrace unavailable on this platform>\n", stderr);
#endif

    report_other_thread_stacks();

    std::fputs("=======================================================\n", stderr);
    std::fflush(stderr);
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

LONG WINAPI seh_filter(EXCEPTION_POINTERS* info) noexcept
{
    unsigned long const code
        = (info != nullptr && info->ExceptionRecord != nullptr) ? info->ExceptionRecord->ExceptionCode : 0;
    report_crash(seh_reason(code));
    return EXCEPTION_EXECUTE_HANDLER; // let the process terminate
}

void abort_handler(int) noexcept
{
    report_crash("abort() / std::terminate");
    std::signal(SIGABRT, SIG_DFL);
    std::_Exit(3);
}

void install_platform_handlers() noexcept
{
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
#endif
}

void cc::add_crash_context_hook(cc::crash_context_hook hook)
{
    if (hook == nullptr || g_hook_count >= k_max_hooks)
        return;
    g_hooks[g_hook_count++] = hook;
}
