#pragma once

// This is a very lean header with minimal dependencies - easy to include everywhere and low cost.
// For formatted assertions with cc::format support, use <clean-core/common/assertf.hh> instead.
#include <clean-core/common/macros.hh>
#include <clean-core/platform/source_location.hh>

// =========================================================================================================
// CC_ASSERT - Runtime assertion with string literal message
//
// Validates a condition at runtime and triggers a debugger break + abort on failure.
//
// Features:
//   - Simple string literal error messages (no formatting dependencies)
//   - Automatic source location capture (file, line, function)
//   - Debugger integration: breaks into debugger when attached, otherwise aborts
//   - Expression stringification for clear error reporting
//   - Active in debug and release-with-debug-info builds by default (we believe in more checks)
//   - Minimal dependencies - does not include string or format headers
//
// When assertions are active:
//   Assertions are enabled in CC_DEBUG and CC_RELWITHDEBINFO builds.
//   In CC_RELEASE builds, assertions are disabled unless CC_ENABLE_ASSERT_IN_RELEASE is defined.
//
// What assertions are for:
//   Assertions protect INVARIANTS, PRECONDITIONS, and POSTCONDITIONS.
//   They catch PROGRAMMER ERRORS early during development.
//
// What assertions are NOT for:
//   - NOT for user input validation
//   - NOT for exceptional error handling
//   - NOT for common/expected error conditions
//
// Error handling strategy:
//   - Assertions      -> programmer errors, violated invariants/preconditions/postconditions
//   - Exceptions      -> exceptional & nonlocal error handling
//   - result<T, E>    -> common/expected error handling
//
// Important:
//   Assertions can be semantically equivalent to std::terminate().
//   NEVER trigger assertions based on user input or external conditions!
//   Production builds can provide a custom assertion handler to prevent data loss.
//
// Usage:
//   CC_ASSERT(ptr != nullptr, "pointer must not be null");
//   CC_ASSERT(size > 0, "size must be positive");
//   CC_ASSERT(idx < array.size(), "index out of bounds");
//
//   void process(int* data, size_t count)
//   {
//       CC_ASSERT(data != nullptr, "data pointer must be valid"); // precondition
//       CC_ASSERT(count > 0, "count must be positive");           // precondition
//       // ... process data ...
//       CC_ASSERT(result_valid(), "postcondition violated");      // postcondition
//   }
//
// Note:
//   For formatted messages with arguments, use CC_ASSERTF from <clean-core/common/assertf.hh>
//
// Rationale:
//   Unlike standard assert(), CC_ASSERT provides:
//     - Better debugger integration (breaks at assertion site, not in abort())
//     - Configurable behavior across build configurations
//     - Source location without macros like __FILE__ and __LINE__
//     - Minimal dependencies for use in low-level code
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
// Triggers a debugger break if a debugger is attached, otherwise does nothing.
//
// Usage:
//   CC_DEBUG_BREAK(); // breaks into debugger if attached
//
//   if (some_error_condition)
//   {
//       log_error("critical error detected");
//       CC_DEBUG_BREAK();
//   }
//
// Rationale:
//   - Safely breaks into the debugger without crashing when no debugger is present
//   - Executes inline (not in a function) so debugger breaks at the exact location
//   - Platform-specific implementation ensures correct debugger interaction
//   - Useful for investigating unexpected conditions without full assertion failure
//
#define CC_DEBUG_BREAK() CC_IMPL_DEBUG_BREAK()

// =========================================================================================================
// CC_BREAK_AND_ABORT - Debug break followed by program termination
//
// Triggers a debugger break (if attached) then unconditionally aborts the program.
//
// Usage:
//   CC_BREAK_AND_ABORT(); // break into debugger, then terminate
//
//   if (unrecoverable_error())
//   {
//       log_fatal("cannot continue");
//       CC_BREAK_AND_ABORT();
//   }
//
// Rationale:
//   - Combines debugger investigation opportunity with guaranteed termination
//   - Used by CC_ASSERT after logging assertion details
//   - Ensures program doesn't continue in an invalid state
//   - Debugger break happens first to allow inspection before termination
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

#ifdef CC_COMPILER_MSVC

// __debugbreak() terminates immediately without an attached debugger
#define CC_IMPL_DEBUG_BREAK() (::cc::impl::is_debugger_connected() ? __debugbreak() : void(0))

#elif defined(CC_COMPILER_POSIX)

// __builtin_trap() causes an illegal instruction and crashes without an attached debugger
// we use a SIGTRAP to signal a trace/breakpoint
// the _trap is technically not correct because a BREAKpoint is recoverable
// the use in CC_ASSERT is simply to provide a cleaner debugging experience
// and is followed by an abort anyways
// NOTE: we don't want to pull in any posix header here, so we simply declare raise
//       SIGTRAP is 5 according to https://man7.org/linux/man-pages/man7/signal.7.html
extern "C" int raise(int) noexcept;
#define CC_IMPL_DEBUG_BREAK() (::cc::impl::is_debugger_connected() ? (void)::raise(5) : void(0))

#else

#define CC_IMPL_DEBUG_BREAK() void(0)

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
