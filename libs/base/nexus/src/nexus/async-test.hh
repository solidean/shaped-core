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
#include <nexus/tests/invoke_tests.hh>
#include <nexus/tests/typed_value.hh>

#include <type_traits>
#include <typeindex>
#include <utility> // std::index_sequence

/// Options for nx::async_invoke_tests_in_parallel.
struct nx::parallel_invocation_options
{
    /// How many children may run at once; 0 bounds them only by the scheduler.
    isize max_concurrent = 0;
};

namespace nx::impl
{
// Hand nexus the graph an ASYNC_TEST body produced.
void submit_test_async(async_test_sink& sink, cc::shared_async<cc::unit> root);

// The engine behind both async invocations; the boxed arguments live in its frame until every child has resolved.
cc::shared_async<invocation_result> async_invoke_tests_impl(cc::string name,
                                                            cc::vector<std::type_index> signature,
                                                            cc::vector<typed_value> boxes,
                                                            bool in_parallel,
                                                            isize max_concurrent);

template <class... Args>
cc::shared_async<invocation_result> async_invoke_tests_boxed(cc::string_view name,
                                                             bool in_parallel,
                                                             isize max_concurrent,
                                                             Args... args)
{
    static_assert(sizeof...(Args) >= 1, "an async invocation needs at least one argument (the join key)");
    auto boxes = cc::vector<typed_value>();
    boxes.reserve(sizeof...(Args));
    (boxes.push_back(typed_value::create(cc::move(args))), ...);
    return async_invoke_tests_impl(cc::string(name), cc::arg_types_of(cc::signature<void(Args...)>{}), cc::move(boxes),
                                   in_parallel, max_concurrent);
}

template <class... A, std::size_t... I>
cc::shared_async<cc::unit> call_async_invocable(cc::shared_async<cc::unit> (*fn)(A...),
                                                cc::span<typed_value*> inputs,
                                                std::index_sequence<I...>)
{
    CC_ASSERT(inputs.size() == sizeof...(A), "invoked with the wrong number of arguments");
    return fn(inputs[I]->template get<A>()...);
}

// Wraps an ASYNC_INVOCABLE_TEST body into the erased invoker the registry stores.
// Parameters follow INVOCABLE_TEST's rule: by value or const&, never a mutable reference, since the boxes are shared.
template <class... A>
cc::unique_function<void(cc::span<typed_value*>, async_test_sink&)> make_async_test_invoker(
    cc::shared_async<cc::unit> (*fn)(A...))
{
    static_assert(((!std::is_lvalue_reference_v<A> || std::is_const_v<std::remove_reference_t<A>>) && ...),
                  "ASYNC_INVOCABLE_TEST parameters must not be mutable lvalue references; use a value or a const& "
                  "(arguments are shared, read-only inputs)");
    return [fn](cc::span<typed_value*> inputs, async_test_sink& sink)
    { submit_test_async(sink, call_async_invocable(fn, inputs, std::index_sequence_for<A...>{})); };
}

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

#define NX_IMPL_ASYNC_INVOCABLE_TEST(name, unique_id, params, ...)                                                     \
    static ::cc::shared_async<::cc::unit> CC_MACRO_JOIN(_nx_async_invocable_fn_, unique_id) params;                    \
    static const bool CC_MACRO_JOIN(_nx_async_invocable_reg_, unique_id)                                               \
        = (::nx::impl::register_async_invocable_test(                                                                  \
               name,                                                                                                   \
               []()                                                                                                    \
               {                                                                                                       \
                   using namespace nx::config;                                                                         \
                   return ::nx::impl::merge_config(__VA_ARGS__);                                                       \
               }(),                                                                                                    \
               ::cc::arg_types_of(::cc::signature_of<decltype(&CC_MACRO_JOIN(_nx_async_invocable_fn_, unique_id))>{}), \
               ::nx::impl::make_async_test_invoker(&CC_MACRO_JOIN(_nx_async_invocable_fn_, unique_id)),                \
               cc::source_location::current()),                                                                        \
           true);                                                                                                      \
    static ::cc::shared_async<::cc::unit> CC_MACRO_JOIN(_nx_async_invocable_fn_, unique_id) params

// An INVOCABLE_TEST whose body is a coroutine.
// Matched by the same decayed signature as a synchronous invocable, so one invocation reaches both kinds — but only an
// async invocation can run it, and the synchronous nx::invoke_tests asserts when its matched set holds one.
// Parameters by value or const&: an async invocation keeps the boxed arguments alive until every child has resolved.
//
//   ASYNC_INVOCABLE_TEST("sg stream - an upload settles", (sg::context_handle const& h))
//   {
//       auto stream = h->stream.bytes_to_buffer(buf, data);
//       co_await stream.completion();
//       CHECK(stream.is_complete());
//   }
#define ASYNC_INVOCABLE_TEST(name, params, ...) NX_IMPL_ASYNC_INVOCABLE_TEST(name, __COUNTER__, params, __VA_ARGS__)

namespace nx
{
/// Run every invocable matching `args...`, sync or async, one after another, awaiting each; co_await it from an async body.
///
/// A child's own asks are arranged rather than inherited: `main_thread` runs it on main, and its exclusion tags are
/// taken from the phase around it.
/// What stays refused, at dispatch: a tagged child under a chain that already holds a tag, an `exclusive()` child under
/// a driver that is not `exclusive()`, and a scheduler mode the driver does not share.
/// Cold, like every coroutine: nothing is matched or run until it is awaited.
template <class... Args>
[[nodiscard]] cc::shared_async<invocation_result> async_invoke_tests_in_sequence(cc::string_view name, Args... args)
{
    return impl::async_invoke_tests_boxed(name, false, 0, cc::move(args)...);
}

/// Start every invocable matching `args...`, then await them all.
///
/// The same asks as the serial form, plus one: an `exclusive()` child is refused, since alone among its siblings is not
/// something a parallel invocation can give it.
/// Children sharing a tag still exclude one another, through the phase's lock for it.
/// Reports keep match order whatever order the children finished in, and under -j1 the children run one at a time.
template <class... Args>
[[nodiscard]] cc::shared_async<invocation_result> async_invoke_tests_in_parallel(cc::string_view name, Args... args)
{
    return impl::async_invoke_tests_boxed(name, true, 0, cc::move(args)...);
}

/// The same, with at most `options.max_concurrent` children running at once.
template <class... Args>
[[nodiscard]] cc::shared_async<invocation_result> async_invoke_tests_in_parallel(cc::string_view name,
                                                                                 parallel_invocation_options options,
                                                                                 Args... args)
{
    return impl::async_invoke_tests_boxed(name, true, options.max_concurrent, cc::move(args)...);
}
} // namespace nx

// EXAMPLE with a coroutine body, baking in the same three: the example bucket, main_thread and exclusive().
// The body starts on main and stays there until it hops away, so a windowed example needs nothing, and a console one
// that wants compute says `co_await cc::async_resume_on_compute();`.
// no_scheduler is refused, as on every async test; an example installing its own scheduler stays a sync EXAMPLE.
#define ASYNC_EXAMPLE(name, ...) \
    NX_IMPL_ASYNC_TEST(name, __COUNTER__, example, main_thread, exclusive() __VA_OPT__(, ) __VA_ARGS__)

// BENCHMARK with a coroutine body: the benchmark bucket (which runs alone) and main_thread.
// Setup can await instead of blocking, and nx::bench::run_async in nexus/bench/run_async.hh measures async work itself.
#define ASYNC_BENCHMARK(name, ...) \
    NX_IMPL_ASYNC_TEST(name, __COUNTER__, benchmark, main_thread __VA_OPT__(, ) __VA_ARGS__)
