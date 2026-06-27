#include "assert.hh"

#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/asserts.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/stacktrace.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef CC_COMPILER_MSVC
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent() noexcept;
#endif

#ifdef CC_COMPILER_POSIX
#include <unistd.h>

#include <cstring>
#endif

#ifdef CC_OS_APPLE
#include <sys/sysctl.h>
#endif

namespace
{
// Global stack of assertion handlers
// NOTE: This is not thread-safe and must be externally synchronized
cc::vector<cc::unique_function<void(cc::impl::assertion_info const&)>> g_assertion_handlers;

// Default assertion handler implementation
void default_assert_handler(cc::impl::assertion_info const& info)
{
    auto const write_str = [](cc::string const& s) { std::cerr.write(s.data(), std::streamsize(s.size())); };
    std::cerr << "Assertion failed: ";
    write_str(info.expression);
    std::cerr << "\n  Message: ";
    write_str(info.message);
    std::cerr << '\n';
    std::cerr << "  Location: " << info.location.file_name() << ':' << info.location.line() << ':'
              << info.location.column() << " (" << info.location.function_name() << ")\n";

    // Print stacktrace. Only the real std::stacktrace can render frames; on toolchains without
    // <stacktrace> (Emscripten / WASI) cc::stacktrace is an empty stub, so say so instead.
    std::cerr << "\nStacktrace:\n";
#if CC_HAS_STACKTRACE
    auto trace = cc::stacktrace::current();
    std::cerr << std::to_string(trace) << '\n';
#else
    std::cerr << "<stacktrace unavailable on this platform>\n";
#endif
}
} // namespace

void cc::impl::push_assertion_handler(cc::unique_function<void(assertion_info const&)> handler)
{
    g_assertion_handlers.push_back(cc::move(handler));
}

void cc::impl::pop_assertion_handler()
{
    if (!g_assertion_handlers.empty())
        g_assertion_handlers.remove_back();
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
    assertion_info const info{
        .expression = cc::string(expression),
        .message = cc::string(message),
        .location = location,
    };

    // Call the topmost handler if available, otherwise use default handler
    if (!g_assertion_handlers.empty())
    {
        g_assertion_handlers.back()(info);
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
#ifdef CC_COMPILER_MSVC
    return ::IsDebuggerPresent() != 0;
#elif defined(CC_OS_LINUX)
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
#elif defined(CC_OS_APPLE)
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
