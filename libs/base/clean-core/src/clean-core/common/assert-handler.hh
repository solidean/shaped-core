#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>

namespace cc::impl
{
// Customizable assertion handler system
//
// The handler stack is per-thread: a handler covers assertions on the thread that pushed it, and nothing else.
// That is what makes a throwing handler usable at all — see cc::impl::set_fallback_assertion_handler for the cross-thread story.
//
// Usage example:
//   {
//       auto handler = cc::impl::scoped_assertion_handler([](cc::impl::assertion_info const& info) {
//           log_assertion_failure(info);
//           throw assertion_failure_exception{info.message};
//       });
//
//       // Any assertions in this scope will use the custom handler
//       risky_operation();
//   } // handler is automatically popped here

struct assertion_info
{
    cc::string expression;
    cc::string message;
    cc::source_location location;
};

// Push a custom assertion handler onto the CALLING THREAD's handler stack
// The handler will be called for assertion failures on that thread until it is popped, and never for another thread's
// Handlers are allowed to throw exceptions as a way to unwind to some recovery point
// This can turn assertion failures into less serious issues in production
// (while this should never be the default in dev, it is valid in production)
void push_assertion_handler(cc::unique_function<void(assertion_info const&)> handler);

// Pop the topmost assertion handler from the calling thread's stack
// NOTE: Be careful with this when using throwing handlers for recovery
//       Ensure each push is matched with a pop even if an exception is thrown
//       (prefer using scoped_assertion_handler for automatic cleanup)
void pop_assertion_handler();

// Handler consulted when the failing thread's own stack is empty — the process-wide default in place of the built-in one.
// It must be callable from any thread at any time, so it is a plain function pointer rather than a stack:
// worker threads pick up work that no thread pushed a handler for, and there is nothing thread-scoped to consult there.
// Throwing from it is a bad idea: it fires on threads nobody scoped, where nothing between it and the thread function catches.
// Returns the previous handler, so a caller can restore it; null means the built-in one.
using fallback_assertion_handler_fn = void (*)(assertion_info const&);
fallback_assertion_handler_fn set_fallback_assertion_handler(fallback_assertion_handler_fn handler);

// RAII wrapper for pushing/popping assertion handlers
struct scoped_assertion_handler
{
    explicit scoped_assertion_handler(cc::unique_function<void(assertion_info const&)> handler);
    ~scoped_assertion_handler();

    scoped_assertion_handler(scoped_assertion_handler const&) = delete;
    scoped_assertion_handler& operator=(scoped_assertion_handler const&) = delete;
    scoped_assertion_handler(scoped_assertion_handler&&) = delete;
    scoped_assertion_handler& operator=(scoped_assertion_handler&&) = delete;
};

// RAII wrapper for setting/restoring the fallback handler
// Nesting is fine and restores in reverse order, but the scopes must nest on ONE thread — the handler itself is process-wide
struct scoped_fallback_assertion_handler
{
    explicit scoped_fallback_assertion_handler(fallback_assertion_handler_fn handler);
    ~scoped_fallback_assertion_handler();

    scoped_fallback_assertion_handler(scoped_fallback_assertion_handler const&) = delete;
    scoped_fallback_assertion_handler& operator=(scoped_fallback_assertion_handler const&) = delete;
    scoped_fallback_assertion_handler(scoped_fallback_assertion_handler&&) = delete;
    scoped_fallback_assertion_handler& operator=(scoped_fallback_assertion_handler&&) = delete;

private:
    fallback_assertion_handler_fn _previous;
};
} // namespace cc::impl
