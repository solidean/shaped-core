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
// C++ needs at least one co_ keyword to make a body a coroutine.
// A body that awaits nothing therefore ends in a bare `co_return;`, or stays a plain body that RETURNS the graph to
// await — the pre-coroutine spelling, still supported:
//
//   ASYNC_TEST("...") { return cc::make_async_lazy<cc::unit>(/* ... */); }
//
// **A returned graph must be COLD** — a `make_async_lazy` root, not one already scheduled or resolved — since nexus
// stamps this test's context onto it as it schedules it, and that stamp only happens on a cold node.
// A coroutine body is cold by construction, so the rule binds only the returning form.
//
// Two further limits, both deliberate for now:
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
    static_assert(std::is_same_v<decltype((*fn)()), cc::shared_async<cc::unit>>,
                  "an ASYNC_TEST body must co_return nothing, or return cc::shared_async<cc::unit>; wrap a graph of "
                  "another type in a make_async_lazy<cc::unit> that requires it");
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
