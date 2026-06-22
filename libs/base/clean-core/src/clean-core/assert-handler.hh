#pragma once

#include <clean-core/macros.hh>
#include <clean-core/source_location.hh>

#include <functional>
#include <string>

namespace cc::impl
{
// Customizable assertion handler system
// NOTE: Handler functions are global state and must be externally synchronized
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
    std::string expression;
    std::string message;
    cc::source_location location;
};

// Push a custom assertion handler onto the handler stack
// The handler will be called for all assertion failures until it is popped
// Handlers are allowed to throw exceptions as a way to unwind to some recovery point
// This can turn assertion failures into less serious issues in production
// (while this should never be the default in dev, it is valid in production)
void push_assertion_handler(std::move_only_function<void(assertion_info const&)> handler);

// Pop the topmost assertion handler from the stack
// NOTE: Be careful with this when using throwing handlers for recovery
//       Ensure each push is matched with a pop even if an exception is thrown
//       (prefer using scoped_assertion_handler for automatic cleanup)
void pop_assertion_handler();

// RAII wrapper for pushing/popping assertion handlers
struct scoped_assertion_handler
{
    explicit scoped_assertion_handler(std::move_only_function<void(assertion_info const&)> handler);
    ~scoped_assertion_handler();

    scoped_assertion_handler(scoped_assertion_handler const&) = delete;
    scoped_assertion_handler& operator=(scoped_assertion_handler const&) = delete;
    scoped_assertion_handler(scoped_assertion_handler&&) = delete;
    scoped_assertion_handler& operator=(scoped_assertion_handler&&) = delete;
};
} // namespace cc::impl
