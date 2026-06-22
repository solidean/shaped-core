#pragma once

#include <clean-core/assert.hh>
#include <clean-core/string.hh>
#include <clean-core/string_view.hh>

#include <format>

// =========================================================================================================
// CC_ASSERTF - Runtime assertion with formatted message
//
// Validates a condition at runtime and triggers a debugger break + abort on failure.
// This is the formatted version of CC_ASSERT, supporting std::format-style arguments.
//
// Features:
//   - Formatted error messages using std::format
//   - Automatic source location capture (file, line, function)
//   - Debugger integration: breaks into debugger when attached, otherwise aborts
//   - Expression stringification for clear error reporting
//   - Active in debug and release-with-debug-info builds by default (we believe in more checks)
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
//   CC_ASSERTF(size > 0, "size must be positive, got {}", size);
//   CC_ASSERTF(idx < array.size(), "index {} out of bounds (size: {})", idx, array.size());
//   CC_ASSERTF(result == expected, "computation failed: {} != {}", result, expected);
//
//   void process(int* data, size_t count)
//   {
//       CC_ASSERTF(data != nullptr, "data pointer is null (count: {})", count); // precondition
//       CC_ASSERTF(count > 0, "count must be positive, got {}", count);         // precondition
//       // ... process data ...
//       CC_ASSERTF(result_valid(), "postcondition violated at index {}", i);    // postcondition
//   }
//
// Note:
//   For simple string literal messages without formatting, prefer CC_ASSERT from <clean-core/assert.hh>
//   as it has minimal dependencies and can be used in low-level code.
//
// Rationale:
//   Unlike CC_ASSERT, CC_ASSERTF provides:
//     - Rich formatting capabilities for diagnostic messages
//     - Runtime values in assertion messages for better debugging
//   Trade-off: Requires string and format headers, which may not be suitable for low-level code
//
#define CC_ASSERTF(cond, msg, ...) CC_IMPL_ASSERTF(cond, msg, ##__VA_ARGS__)

// =========================================================================================================
// CC_ASSERTF_ALWAYS - Always-active assertion with formatted message
//
// Like CC_ASSERTF but remains active in all build configurations, including release builds.
// Use this for critical invariants that must always be checked, even in production.
//
// Usage:
//   CC_ASSERTF_ALWAYS(size <= MAX_SIZE, "exceeded absolute size limit: {} > {}", size, MAX_SIZE);
//   CC_ASSERTF_ALWAYS(fd >= 0, "failed to open file: {} (error: {})", path, errno);
//
#define CC_ASSERTF_ALWAYS(cond, msg, ...) CC_IMPL_ASSERTF_ALWAYS(cond, msg, ##__VA_ARGS__)


// =========================================================================================================
// Implementation details
// =========================================================================================================

// CC_ASSERTF_ALWAYS implementation - always enabled regardless of build configuration
#define CC_IMPL_ASSERTF_ALWAYS(cond, msg, ...)                                                            \
    do                                                                                                    \
    {                                                                                                     \
        if (!(cond)) [[unlikely]]                                                                         \
        {                                                                                                 \
            ::cc::impl::handle_assert_failure(#cond, std::format(msg __VA_OPT__(, ) __VA_ARGS__).c_str(), \
                                              ::cc::source_location::current());                          \
            CC_BREAK_AND_ABORT();                                                                         \
        }                                                                                                 \
    } while (false)

// Assertf implementation - enabled in debug/relwithdebinfo, optionally in release

#if CC_ASSERT_ENABLED

// Delegate to CC_IMPL_ASSERTF_ALWAYS when assertions are enabled
#define CC_IMPL_ASSERTF(cond, msg, ...) CC_IMPL_ASSERTF_ALWAYS(cond, msg, ##__VA_ARGS__)

#else

// In release builds without CC_ENABLE_ASSERT_IN_RELEASE, assertions are stripped
// We still use the format string to ensure it compiles correctly
#define CC_IMPL_ASSERTF(cond, msg, ...)                         \
    do                                                          \
    {                                                           \
        CC_UNUSED(cond);                                        \
        CC_UNUSED(std::format(msg __VA_OPT__(, ) __VA_ARGS__)); \
    } while (false)

#endif
