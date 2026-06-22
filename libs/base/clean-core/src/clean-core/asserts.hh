#pragma once

#include <clean-core/assert.hh>
#include <clean-core/string_view.hh>

// =========================================================================================================
// CC_ASSERTS - Runtime assertion with string_view message
//
// Validates a condition at runtime and triggers a debugger break + abort on failure.
// This is a middle-weight assertion between CC_ASSERT (literals only) and CC_ASSERTF (formatted).
//
// Features:
//   - String view error messages (no formatting, but can accept runtime strings)
//   - Automatic source location capture (file, line, function)
//   - Debugger integration: breaks into debugger when attached, otherwise aborts
//   - Expression stringification for clear error reporting
//   - Active in debug and release-with-debug-info builds by default (we believe in more checks)
//   - Minimal dependencies - only includes string_view, not format
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
//   CC_ASSERTS(ptr != nullptr, "pointer must not be null");
//   CC_ASSERTS(size > 0, "size must be positive");
//   CC_ASSERTS(idx < array.size(), error_message);  // error_message is a string_view
//
//   void process(int* data, size_t count, cc::string_view name)
//   {
//       CC_ASSERTS(data != nullptr, "data pointer must be valid"); // precondition
//       CC_ASSERTS(count > 0, "count must be positive");           // precondition
//       CC_ASSERTS(!name.empty(), name);                           // runtime string_view
//       // ... process data ...
//       CC_ASSERTS(result_valid(), "postcondition violated");      // postcondition
//   }
//
// Note:
//   For simple string literal messages, prefer CC_ASSERT from <clean-core/assert.hh>.
//   For formatted messages with arguments, use CC_ASSERTF from <clean-core/assertf.hh>.
//
// Rationale:
//   Unlike CC_ASSERT, CC_ASSERTS can accept runtime string_view messages.
//   Unlike CC_ASSERTF, CC_ASSERTS does not require the heavy format header.
//
#define CC_ASSERTS(cond, msg) CC_IMPL_ASSERTS(cond, msg)

// =========================================================================================================
// CC_ASSERTS_ALWAYS - Always-active assertion with string_view message
//
// Like CC_ASSERTS but remains active in all build configurations, including release builds.
// Use this for critical invariants that must always be checked, even in production.
//
// Usage:
//   CC_ASSERTS_ALWAYS(critical_ptr != nullptr, "critical invariant violated");
//   CC_ASSERTS_ALWAYS(size <= MAX_SIZE, error_msg);  // error_msg is a string_view
//
#define CC_ASSERTS_ALWAYS(cond, msg) CC_IMPL_ASSERTS_ALWAYS(cond, msg)


// =========================================================================================================
// Implementation details
// =========================================================================================================

namespace cc::impl
{
// Called when an assertion fails (string_view version)
// Prints diagnostic information to stderr
// Note: does not abort, caller must follow with CC_BREAK_AND_ABORT()
CC_COLD_FUNC void handle_assert_failure_sv(char const* expression, cc::string_view message, cc::source_location location);
} // namespace cc::impl

// CC_ASSERTS_ALWAYS implementation - always enabled regardless of build configuration
#define CC_IMPL_ASSERTS_ALWAYS(cond, msg)                                                        \
    do                                                                                           \
    {                                                                                            \
        if (!(cond)) [[unlikely]]                                                                \
        {                                                                                        \
            ::cc::impl::handle_assert_failure_sv(#cond, msg, ::cc::source_location::current()); \
            CC_BREAK_AND_ABORT();                                                                \
        }                                                                                        \
    } while (false)

// Asserts implementation - enabled in debug/relwithdebinfo, optionally in release

#if CC_ASSERT_ENABLED

// Delegate to CC_IMPL_ASSERTS_ALWAYS when assertions are enabled
#define CC_IMPL_ASSERTS(cond, msg) CC_IMPL_ASSERTS_ALWAYS(cond, msg)

#else

// In release builds without CC_ENABLE_ASSERT_IN_RELEASE, assertions are stripped
// We still evaluate the message to ensure it compiles correctly
#define CC_IMPL_ASSERTS(cond, msg) \
    do                             \
    {                              \
        CC_UNUSED(cond);           \
        CC_UNUSED(msg);            \
    } while (false)

#endif
