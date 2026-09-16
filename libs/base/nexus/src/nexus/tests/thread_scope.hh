#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/thread/async_ambient.hh>
#include <nexus/fwd.hh>

// Attributing a thread you started yourself to the test that started it.
//
// A CHECK finds its test through cc::async's ambient context, which rides a cc::async graph on its own.
// A bare thread carries nothing, so a check there belongs to NO test — and nexus fails the run for it rather than dropping it silently.
// These are the door out of that: capture on the test's thread, install on the other one.
//
// Nothing here is needed for work driven by cc::async, which is already attributed.
// Note also what this does NOT buy: in a TEST, opening sections stays with the test's own thread.
// Checks reported here are filed under the section whose pass is running.
// And a REQUIRE here records a failure instead of aborting, since nothing would catch the throw.

/// Attribute everything the calling thread reports to a captured test, for this scope.
/// `captured` must outlive the scope.
struct nx::test_thread_scope
{
    explicit test_thread_scope(cc::async_ambient_handle const& captured) : _scope(captured) {}

    test_thread_scope(test_thread_scope const&) = delete;
    test_thread_scope& operator=(test_thread_scope const&) = delete;

private:
    cc::async_ambient_install_scope _scope;
};

namespace nx::impl
{
/// Whether a report from the calling thread would find a test, running or already finished.
[[nodiscard]] bool has_current_test();
} // namespace nx::impl

namespace nx
{
/// Capture the running test, so a thread started with it can be attributed back.
/// Copy the result into the thread's callable; it is a pointer plus a refcount bump, and safe to hold past the test's end (it then attributes nothing).
[[nodiscard]] inline cc::async_ambient_handle capture_current_test()
{
    return cc::async_ambient_handle();
}

/// Wrap `f` so it runs attributed to the test running right now — the shape for handing work to a thread:
///
///   std::thread t(nx::attributed_to_current_test([&] { CHECK(worker_saw_it); }));
template <class F>
[[nodiscard]] auto attributed_to_current_test(F&& f)
{
    return [captured = capture_current_test(), fn = cc::forward<F>(f)]() mutable
    {
        test_thread_scope const scope(captured);
        fn();
    };
}

/// Run `f` attributed to the calling thread's own test, or to `fallback` when the thread carries none.
///
/// For a callback a library raises from threads of its own, such as a graphics API's validation messages.
/// Under a test the report lands there; anywhere else it lands on whoever installed the callback, rather than failing the run with no test named.
/// `fallback` must be released before its test ends, or the test counts it as async work left running.
template <class F>
void with_fallback_test(cc::async_ambient_handle const& fallback, F&& f)
{
    if (impl::has_current_test())
    {
        f();
        return;
    }
    test_thread_scope const scope(fallback);
    f();
}
} // namespace nx
