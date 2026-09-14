#pragma once

// ASYNC_TEST: a test whose body may co_await.
//
// A separate header on purpose.
// TEST is already an async test underneath — one whose body simply never suspends — so nexus/test.hh has no reason to
// pay for the async templates, and this is the header you include when you want them.
//
//   ASYNC_TEST("cache - resolves a miss")
//   {
//       auto const entry = co_await cache.acquire_async("shader.hlsl");
//       CHECK(entry.is_compiled());
//   }
//
// The body is the graph.
// Nexus schedules it under this test's context and makes the test wait on it, so a park inside parks the TEST rather
// than blocking a worker, and every check it reports still finds this test from whichever thread ran it.
// That holds at any depth: a check inside a coroutine the body awaited, which nexus never saw, is billed here too.
//
// **The body must be a coroutine**, and one that is not fails the test by name.
// C++ needs at least one co_ keyword to make it one, so a body that awaits nothing ends in a bare `co_return;`.
// Nexus places the body before its first line runs — on main for `main_thread`, on the phase's scheduler otherwise —
// and only a coroutine can be placed that way.
//
// Every scheduling ask a TEST takes applies: `main_thread` homes the body to main until it hops away itself,
// `singlethreaded` drives it inline on the run thread, `own_pool(n)` runs it on that pool, and exclusion holds across every suspend.
// `no_scheduler` is refused, since nothing would drive the body.
//
// SKIP and REQUIRE behave as in a TEST, at any depth: the node error their throw becomes is the abort, not a second failure.
//
// Two limits, both deliberate:
//
// * SECTION is not available in an async body, and asserts.
//   The section machinery replays the body once per section path, which is single-threaded state, and an async body runs once.
// * A graph resolving to an ERROR fails the test, naming the error, and is never propagated onward.
//   A test node always resolves to a value, or an exclusivity edge would carry the failure into every test ordered behind it.
//   An awaited dependency that fails is exactly that case: it short-circuits the rest of the body, then fails the test.

#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>

namespace nx::impl
{
// Hand nexus the graph an ASYNC_TEST body produced.
void submit_test_async(async_test_sink& sink, cc::shared_async<cc::unit> root);

// Adapts a body into the erased sink call the registry stores.
template <class F>
cc::unique_function<void(async_test_sink&)> make_async_test_body(F* fn)
{
    static_assert(std::is_same_v<decltype((*fn)()), cc::shared_async<cc::unit>>, "an ASYNC_TEST body must be a "
                                                                                 "coroutine that co_returns nothing");
    return [fn](async_test_sink& sink) { submit_test_async(sink, (*fn)()); };
}
} // namespace nx::impl

#define NX_IMPL_ASYNC_TEST(name, unique_id, ...)                                                \
    static ::cc::shared_async<::cc::unit> CC_MACRO_JOIN(_nx_async_test_fn_, unique_id)();       \
    static const bool CC_MACRO_JOIN(_nx_async_test_reg_, unique_id)                             \
        = (::nx::impl::register_async_test(                                                     \
               name,                                                                            \
               []()                                                                             \
               {                                                                                \
                   using namespace nx::config;                                                  \
                   return ::nx::impl::merge_config(__VA_ARGS__);                                \
               }(),                                                                             \
               ::nx::impl::make_async_test_body(&CC_MACRO_JOIN(_nx_async_test_fn_, unique_id)), \
               cc::source_location::current()),                                                 \
           true);                                                                               \
    static ::cc::shared_async<::cc::unit> CC_MACRO_JOIN(_nx_async_test_fn_, unique_id)()

// A test whose body may co_await; nexus awaits it.
// Config items compose exactly as with TEST.
#define ASYNC_TEST(name, ...) NX_IMPL_ASYNC_TEST(name, __COUNTER__, __VA_ARGS__)
