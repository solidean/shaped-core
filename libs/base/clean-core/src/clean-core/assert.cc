#include "assert.hh"

#include <clean-core/assert-handler.hh>
#include <clean-core/asserts.hh>
#include <clean-core/stacktrace.hh>
#include <clean-core/string.hh>
#include <clean-core/string_view.hh>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef CC_COMPILER_MSVC
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent() noexcept;
#endif

#ifdef CC_COMPILER_POSIX
#include <unistd.h>

#include <cstring>
#endif

namespace
{
// Global stack of assertion handlers
// NOTE: This is not thread-safe and must be externally synchronized
std::vector<std::move_only_function<void(cc::impl::assertion_info const&)>> g_assertion_handlers;

// Default assertion handler implementation
void default_assert_handler(cc::impl::assertion_info const& info)
{
    std::cerr << "Assertion failed: " << info.expression << '\n';
    std::cerr << "  Message: " << info.message << '\n';
    std::cerr << "  Location: " << info.location.file_name() << ':' << info.location.line() << ':'
              << info.location.column() << " (" << info.location.function_name() << ")\n";

    // Print stacktrace
    std::cerr << "\nStacktrace:\n";
    auto trace = cc::stacktrace::current();
    std::cerr << std::to_string(trace) << '\n';
}
} // namespace

void cc::impl::push_assertion_handler(std::move_only_function<void(assertion_info const&)> handler)
{
    g_assertion_handlers.push_back(std::move(handler));
}

void cc::impl::pop_assertion_handler()
{
    if (!g_assertion_handlers.empty())
        g_assertion_handlers.pop_back();
}

cc::impl::scoped_assertion_handler::scoped_assertion_handler(std::move_only_function<void(assertion_info const&)> handler)
{
    push_assertion_handler(std::move(handler));
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
        .expression = std::string(expression),
        .message = std::string(message.data(), static_cast<size_t>(message.size())),
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
    // Use sysctl to check P_TRACED flag
    extern "C" int sysctl(int*, unsigned int, void*, unsigned long*, void*, unsigned long) noexcept;

    int mib[4] = {1 /* CTL_KERN */, 14 /* KERN_PROC */, 1 /* KERN_PROC_PID */, 0};
    mib[3] = getpid();

    struct kinfo_proc
    {
        char pad[32];
        int p_flag;
    } info{};

    unsigned long size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0)
        return (info.p_flag & 0x00000800 /* P_TRACED */) != 0;

    return false;
#else
    return false;
#endif
}

[[noreturn]] void cc::impl::perform_abort() noexcept
{
    std::abort();
}
