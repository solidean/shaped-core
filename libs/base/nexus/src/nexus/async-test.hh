#pragma once

// ASYNC_TEST: a test whose body hands back a graph instead of running to completion.
//
// A separate header on purpose.
// TEST is already an async test underneath — one whose body simply never suspends — so nexus/test.hh has no reason to
// pay for the async templates, and this is the header you include when you want them.
//
//   ASYNC_TEST("cache - resolves a miss")
//   {
//       auto entry = cache.acquire_async("shader.hlsl");
//       return cc::make_async_lazy<cc::unit>(
//           [entry](cc::async_context<cc::unit>& actx) -> cc::async_step_status
//           {
//               if (!actx.require(entry))
//                   return actx.wait_for_dependencies();
//               CHECK(entry->has_value());
//               return actx.resolve_to_value(cc::unit{});
//           });
//   }
//
// The body runs to its `return` exactly like a TEST body does — same thread, no scheduler bound, its own graphs its own business.
// What it RETURNS is different: nexus schedules that root under this test's context and makes the test wait on it.
// So a park inside the graph parks the test rather than blocking a worker, and every check the graph reports still finds this test from whichever thread ran it.
//
// **The returned root must be COLD** — a `make_async_lazy` graph, not one already scheduled or already resolved.
// Nexus stamps this test's context onto it when it schedules it, and that stamp only happens on a cold node.
//
// Two further limits, both deliberate for now:
//
// * SECTION is not available in an async body, and asserts.
//   The section machinery replays the body once per section path, which is single-threaded state, and an async body runs once.
// * A graph resolving to an ERROR fails the test, naming the error, and is never propagated onward.
//   A test node always resolves to a value, or an exclusivity edge would carry the failure into every test ordered behind it.

#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>

namespace nx::impl
{
// Hand nexus the graph an ASYNC_TEST body produced.
void submit_test_async(async_test_sink& sink, cc::shared_async<cc::unit> root);

// Adapts a body into the erased sink call the registry stores.
template <class F>
cc::unique_function<void(async_test_sink&)> make_async_test_body(F* fn)
{
    static_assert(std::is_same_v<decltype((*fn)()), cc::shared_async<cc::unit>>,
                  "an ASYNC_TEST body must return cc::shared_async<cc::unit>; wrap a graph of another type in a "
                  "make_async_lazy<cc::unit> that requires it");
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

// A test whose body returns the graph nexus should await.
// Config items compose exactly as with TEST.
#define ASYNC_TEST(name, ...) NX_IMPL_ASYNC_TEST(name, __COUNTER__, __VA_ARGS__)
