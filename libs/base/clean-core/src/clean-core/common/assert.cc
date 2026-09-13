#include "assert.hh"

#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/asserts.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/stacktrace.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>

#include <cstdio>
#include <cstdlib>

#ifdef CC_OS_WINDOWS
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent() noexcept;
#endif

#ifndef CC_OS_WINDOWS
#include <unistd.h>

#endif

#ifdef CC_ABI_DARWIN
#include <sys/sysctl.h>
#endif

namespace
{
// Per-thread stack of assertion handlers.
// Per-thread rather than global because a handler is scoped to a call, and calls belong to threads:
// a global stack would expose one thread's throwing recovery handler to every other thread's asserts.
thread_local cc::vector<cc::unique_function<void(cc::impl::assertion_info const&)>> t_assertion_handlers;

// Consulted when the failing thread's stack is empty, so work on a thread nobody pushed a handler for is still covered.
cc::atomic<cc::impl::fallback_assertion_handler_fn> g_fallback_assertion_handler = {nullptr};

// Whether this thread is already inside the default handler.
//
// An assert raised WHILE reporting one would otherwise recurse until the stack ran out, and the two ways in are real:
// recording the failure runs cc::rec's own asserted code, and rendering a stacktrace runs the platform's.
// The inner one is written straight to stderr, because that is the one path here with nothing under it to fail.
thread_local bool t_in_default_handler = false;

// Default assertion handler implementation
void default_assert_handler(cc::impl::assertion_info const& info)
{
    auto const location = cc::format("{}:{}:{} ({})", info.location.file_name(), info.location.line(),
                                     info.location.column(), info.location.function_name());

    if (t_in_default_handler)
    {
        cc::eprint(cc::format("Assertion failed while reporting one: {}\n  Message: {}\n  Location: {}\n",
                              info.expression, info.message, location));
        cc::eflush();
        return;
    }

    t_in_default_handler = true;

    // Recorded as well as printed, so the failure reaches the crash dump, a test's recording and any listener the
    // application installed — not only whichever terminal happened to be attached.
    // The domain captures a stack at error level, so the recording carries one without this asking.
    CC_LOG_ERROR("assertion failed: {} — {} at {}", info.expression, info.message, location);

    // stderr stays the primary account of a dying process: it always works, it needs no listener to have been
    // installed, and it is the only one of the two that renders the stack as text.
    cc::eprint(
        cc::format("Assertion failed: {}\n  Message: {}\n  Location: {}\n", info.expression, info.message, location));

    // CC_HAS_STACKTRACE is whether frames can be rendered at all, which is not the same question as <stacktrace>:
    // Emscripten has no such header and still renders them, while WASI has neither and falls back to the empty stub.
    // cc::to_string hides which backend answered.
#if CC_HAS_STACKTRACE
    auto const trace = cc::to_string(cc::stacktrace::current());
    cc::eprint("\nStacktrace:\n");
    cc::eprint(cc::string_view(trace.data(), cc::isize(trace.size())));
    cc::eprint("\n");
#else
    cc::eprint("\nStacktrace:\n<stacktrace unavailable on this platform>\n");
#endif
    cc::eflush();

    // Before the abort outside, or the event dies in a chunk nobody drained.
    if (cc::rec::is_initialized())
        cc::rec::flush_blocking();

    t_in_default_handler = false;
}
} // namespace

void cc::impl::push_assertion_handler(cc::unique_function<void(assertion_info const&)> handler)
{
    t_assertion_handlers.push_back(cc::move(handler));
}

void cc::impl::pop_assertion_handler()
{
    if (!t_assertion_handlers.empty())
        t_assertion_handlers.remove_back();
}

cc::impl::fallback_assertion_handler_fn cc::impl::set_fallback_assertion_handler(fallback_assertion_handler_fn handler)
{
    return g_fallback_assertion_handler.exchange(handler, cc::memory_order_relaxed);
}

cc::impl::scoped_fallback_assertion_handler::scoped_fallback_assertion_handler(fallback_assertion_handler_fn handler)
  : _previous(set_fallback_assertion_handler(handler))
{
}

cc::impl::scoped_fallback_assertion_handler::~scoped_fallback_assertion_handler()
{
    set_fallback_assertion_handler(_previous);
}

cc::impl::scoped_assertion_handler::scoped_assertion_handler(cc::unique_function<void(assertion_info const&)> handler)
{
    push_assertion_handler(cc::move(handler));
}

cc::impl::scoped_assertion_handler::~scoped_assertion_handler()
{
    pop_assertion_handler();
}

// Overload for string_view (canonical implementation)
CC_COLD_FUNC void cc::impl::handle_assert_failure_sv(char const* expression,
                                                     cc::string_view message,
                                                     cc::source_location location)
{
    assertion_info const info = {
        .expression = cc::string(expression),
        .message = cc::string(message),
        .location = location,
    };

    // This thread's topmost handler wins; else the process-wide fallback; else the built-in one
    if (!t_assertion_handlers.empty())
    {
        t_assertion_handlers.back()(info);
    }
    else if (auto const fallback = g_fallback_assertion_handler.load(cc::memory_order_relaxed))
    {
        fallback(info);
    }
    else
    {
        default_assert_handler(info);
    }

    // no abort here, it's outside
}

// Overload for string literals (used by assert.hh) - delegates to string_view version
CC_COLD_FUNC void cc::impl::handle_assert_failure(char const* expression, char const* message, cc::source_location location)
{
    handle_assert_failure_sv(expression, cc::string_view(message), location);
}

bool cc::impl::is_debugger_connected() noexcept
{
#ifdef CC_OS_WINDOWS
    return ::IsDebuggerPresent() != 0;
#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)
    // Check /proc/self/status for TracerPid
    if (auto* f = std::fopen("/proc/self/status", "r"))
    {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f))
        {
            if (std::strncmp(buf, "TracerPid:", 10) == 0)
            {
                int pid = 0;
                std::sscanf(buf + 10, "%d", &pid);
                std::fclose(f);
                return pid != 0;
            }
        }
        std::fclose(f);
    }
    return false;
#elif defined(CC_ABI_DARWIN)
    // Ask the kernel for our own process info and test the P_TRACED flag — the
    // canonical macOS debugger check (Apple Technical Q&A QA1361). Uses the real
    // kinfo_proc from <sys/sysctl.h> rather than a hand-rolled layout.
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info = {};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0)
        return (info.kp_proc.p_flag & P_TRACED) != 0;

    return false;
#else
    return false;
#endif
}

[[noreturn]] void cc::impl::perform_abort() noexcept
{
    std::abort();
}
