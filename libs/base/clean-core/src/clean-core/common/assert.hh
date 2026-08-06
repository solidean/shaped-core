#pragma once

// The leanest of the assertion headers, so it is cheap to include everywhere.
#include <clean-core/common/macros.hh>
#include <clean-core/platform/source_location.hh>

// =========================================================================================================
// CC_ASSERT - Runtime assertion with string literal message
//
// Validates a condition at runtime, and triggers a debugger break plus abort on failure.
// The message is a plain string literal, so this header pulls in no string or format machinery.
// For a formatted message use CC_ASSERTF from <clean-core/common/assertf.hh>, and for a runtime cc::string_view message CC_ASSERTS from <clean-core/common/asserts.hh>.
// Source location and the stringified expression are captured automatically.
//
// Whether assertions are active is CC_ASSERT_ENABLED, derived in <clean-core/common/macros.hh>.
// It is on in debug and release-with-debug-info builds, and off in release unless CC_ENABLE_ASSERT_IN_RELEASE is defined.
//
// Assertions protect INVARIANTS, PRECONDITIONS and POSTCONDITIONS — they catch PROGRAMMER ERRORS early.
// They are NOT for validating user input, for exceptional error handling, or for common and expected error conditions.
// The rest of the strategy: exceptions for exceptional and nonlocal error handling, result<T, E> for expected errors.
//
// A failing assertion can be semantically equivalent to std::terminate().
// So NEVER trigger one from user input or an external condition.
// A production build can install a custom assertion handler to prevent data loss.
//
// Usage:
//   CC_ASSERT(ptr != nullptr, "pointer must not be null");
//   CC_ASSERT(idx < array.size(), "index out of bounds");
//
#define CC_ASSERT(cond, msg) CC_IMPL_ASSERT(cond, msg)

// =========================================================================================================
// CC_ASSERT_ALWAYS - Always-active assertion
//
// Like CC_ASSERT but remains active in all build configurations, including release builds.
// Use this for critical invariants that must always be checked, even in production.
//
// Usage:
//   CC_ASSERT_ALWAYS(critical_ptr != nullptr, "critical invariant violated");
//   CC_ASSERT_ALWAYS(size <= MAX_SIZE, "exceeded absolute size limit");
//
// Note:
//   For formatted messages with arguments, use CC_ASSERTF_ALWAYS from <clean-core/common/assertf.hh>
//
#define CC_ASSERT_ALWAYS(cond, msg) CC_IMPL_ASSERT_ALWAYS(cond, msg)

// =========================================================================================================
// CC_UNREACHABLE - Marks a code path as unreachable
//
// Triggers an assertion failure with the given message, then invokes undefined behavior
// via a compiler builtin to allow optimizations based on unreachable code paths.
//
// Usage:
//   CC_UNREACHABLE("should not reach here");
//
//   default:
//       CC_UNREACHABLE("unhandled enum value");
//
#define CC_UNREACHABLE(msg)          \
    do                               \
    {                                \
        CC_IMPL_ASSERT(false, msg);  \
        CC_IMPL_BUILTIN_UNREACHABLE; \
    } while (0)

// =========================================================================================================
// CC_DEBUG_BREAK - Conditional debugger breakpoint
//
// Triggers a debugger break if a debugger is attached, and does nothing otherwise — so it never crashes a debugger-less run.
// It expands inline rather than into a function call, so the debugger stops at the exact location.
//
// Usage:
//   CC_DEBUG_BREAK(); // breaks into the debugger if one is attached
//
#define CC_DEBUG_BREAK() CC_IMPL_DEBUG_BREAK()

// =========================================================================================================
// CC_BREAK_AND_ABORT - Debug break followed by program termination
//
// Triggers a debugger break (if one is attached) and then unconditionally aborts, so execution never continues in an invalid state.
// The break happens first, to allow inspection before termination.
// This is what CC_ASSERT runs after logging the assertion details.
//
// Usage:
//   CC_BREAK_AND_ABORT(); // break into the debugger, then terminate
//
#define CC_BREAK_AND_ABORT() (CC_DEBUG_BREAK(), ::cc::impl::perform_abort())


// =========================================================================================================
// Implementation details
// =========================================================================================================

namespace cc::impl
{
// Called when an assertion fails
// Prints diagnostic information to stderr
// Note: does not abort, caller must follow with CC_BREAK_AND_ABORT()
CC_COLD_FUNC void handle_assert_failure(char const* expression, char const* message, cc::source_location location);

// Checks if a debugger is currently attached to the process
// Platform-specific implementation (Windows: IsDebuggerPresent, Linux: /proc, macOS: sysctl)
bool is_debugger_connected() noexcept;

// Terminates the program
// Wrapper around std::abort() to allow future customization
[[noreturn]] void perform_abort() noexcept;
} // namespace cc::impl

// Platform-specific debugger break implementation
// The debugger should break right in the assert macro, so this cannot hide in a function call

#ifdef CC_OS_WINDOWS

// __debugbreak() terminates immediately without an attached debugger (works on cl and clang-cl)
#define CC_IMPL_DEBUG_BREAK() (::cc::impl::is_debugger_connected() ? __debugbreak() : void(0))

#else

// __builtin_trap() causes an illegal instruction and crashes without an attached debugger
// we use a SIGTRAP to signal a trace/breakpoint
// the _trap is technically not correct because a BREAKpoint is recoverable
// the use in CC_ASSERT is simply to provide a cleaner debugging experience
// and is followed by an abort anyways
// NOTE: we don't want to pull in any posix header here, so we simply declare raise.
//       Its exception specification must match the platform libc: bionic (Android) does NOT mark raise noexcept, unlike glibc/musl/Darwin.
//       A noexcept here would then clash with bionic's <signal.h>.
//       SIGTRAP is 5 according to https://man7.org/linux/man-pages/man7/signal.7.html
#if defined(CC_OS_ANDROID)
extern "C" int raise(int);
#else
extern "C" int raise(int) noexcept;
#endif
#define CC_IMPL_DEBUG_BREAK() (::cc::impl::is_debugger_connected() ? (void)::raise(5) : void(0))

#endif

// CC_ASSERT_ALWAYS implementation - always enabled regardless of build configuration
#define CC_IMPL_ASSERT_ALWAYS(cond, msg)                                                     \
    do                                                                                       \
    {                                                                                        \
        if (!(cond)) [[unlikely]]                                                            \
        {                                                                                    \
            ::cc::impl::handle_assert_failure(#cond, msg, ::cc::source_location::current()); \
            CC_BREAK_AND_ABORT();                                                            \
        }                                                                                    \
    } while (false)

// Assert implementation - enabled in debug/relwithdebinfo, optionally in release

#if CC_ASSERT_ENABLED

// Delegate to CC_IMPL_ASSERT_ALWAYS when assertions are enabled
#define CC_IMPL_ASSERT(cond, msg) CC_IMPL_ASSERT_ALWAYS(cond, msg)

#else

// In release builds without CC_ENABLE_ASSERT_IN_RELEASE, assertions are stripped
// We still evaluate the message to ensure it compiles correctly
#define CC_IMPL_ASSERT(cond, msg) \
    do                            \
    {                             \
        CC_UNUSED(cond);          \
        CC_UNUSED(msg);           \
    } while (false)

#endif
