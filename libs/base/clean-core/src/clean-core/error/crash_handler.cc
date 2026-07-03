#include "crash_handler.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/platform/stacktrace.hh>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#if CC_HAS_STACKTRACE
#include <string> // std::to_string(std::stacktrace) is the only renderer that produces frame text
#endif

#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>
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

// Shared best-effort reporter: fault description, registered context, then a stacktrace.
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
    std::fputs("\nstacktrace:\n", stderr);
    auto const text = std::to_string(cc::stacktrace::current());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
#else
    std::fputs("\n<stacktrace unavailable on this platform>\n", stderr);
#endif

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
