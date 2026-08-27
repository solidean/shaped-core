#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/common/traits.hh> // cc::signature_of / cc::arg_types_of
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/source_location.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/check.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/invoke_tests.hh>
#include <nexus/tests/section.hh>
#include <nexus/tests/typed_invoke.hh>
#include <nexus/tests/typed_value.hh>

#include <typeindex>

namespace nx::impl
{
void register_test(char const* name, config::cfg test_config, void (*fn)(), cc::source_location loc);

// Registers an ASYNC_TEST body: `fn` runs the body and deposits the graph it wants awaited in the sink.
// Declared here rather than in nexus/async-test.hh so this header names no async type at all — that header carries the macro.
void register_async_test(char const* name,
                         config::cfg test_config,
                         cc::unique_function<void(async_test_sink&)> fn,
                         cc::source_location loc);

// Registers an invocable (inert) test.
// `signature` is the decayed argument-type join key, and `fn` runs the body with args sourced from typed_value slots.
// Non-template so test.hh stays light.
void register_invocable_test(char const* name,
                             config::cfg test_config,
                             cc::vector<std::type_index> signature,
                             cc::unique_function<void(cc::span<nx::typed_value*>)> fn,
                             cc::source_location loc);

// Wraps a `void(A...)` test body into the type-erased invoker stored in the registry.
// It unpacks the typed_value slots back into the concrete argument types and calls the body.
// A mutable lvalue-reference parameter is rejected, because matching decays the signature and the boxed args are shared read-only inputs.
// A `T&` would silently share and mutate one box across instances, so use `T` or `T const&`.
template <class... A>
cc::unique_function<void(cc::span<nx::typed_value*>)> make_test_invoker(void (*fn)(A...))
{
    static_assert(((!std::is_lvalue_reference_v<A> || std::is_const_v<std::remove_reference_t<A>>) && ...),
                  "INVOCABLE_TEST parameters must not be mutable lvalue references; use a value or a const& "
                  "(arguments are shared, read-only inputs)");
    return [fn](cc::span<nx::typed_value*> inputs)
    { ::nx::impl::invoke_with_values(fn, inputs, ::cc::signature<void(A...)>{}); };
}
} // namespace nx::impl

#define NX_IMPL_TEST(name, unique_id, ...)                                               \
    static void CC_MACRO_JOIN(_nx_test_fn_, unique_id)();                                \
    static const bool CC_MACRO_JOIN(_nx_test_reg_, unique_id)                            \
        = (::nx::impl::register_test(                                                    \
               name,                                                                     \
               []()                                                                      \
               {                                                                         \
                   using namespace nx::config;                                           \
                   return ::nx::impl::merge_config(__VA_ARGS__);                         \
               }(),                                                                      \
               &CC_MACRO_JOIN(_nx_test_fn_, unique_id), cc::source_location::current()), \
           true);                                                                        \
    static void CC_MACRO_JOIN(_nx_test_fn_, unique_id)()

#define TEST(name, ...) NX_IMPL_TEST(name, __COUNTER__, __VA_ARGS__)

// A PGO benchmark: a test in the pgo_benchmark bucket that reports metrics via nx::pgo (see guide.hh).
// Swept only via --pgo-benchmarks, or named exactly, and never in a normal run.
// Extra config items compose as with TEST, e.g. PGO_BENCHMARK("name", seed(42)).
#define PGO_BENCHMARK(name, ...) NX_IMPL_TEST(name, __COUNTER__, pgo_benchmark __VA_OPT__(, ) __VA_ARGS__)

// An example: a runnable demonstration of an API in practice, in the example bucket.
// Swept only via --examples, or named exactly, and never in a normal run; `dev.py example <match>` runs exactly one.
// Its body needs no CHECK — an example shows how a library FEELS to use, which is precisely what a test's testability bias filters out.
// A failing CHECK still fails, so an example may assert where asserting is part of the demonstration.
//
// The name is a slash path, because it doubles as the gallery entry and the screenshot slug:
//
//   EXAMPLE("blob-cache/put-and-get")
//
// `main_thread` is baked in: bodies run on the thread nx::run was entered on, one at a time, which is what a window or a device context usually needs.
// The run still installs an ambient async scheduler, so an example may use asyncs without standing up a pool of its own —
// `no_scheduler` is the trailing config item for the example that wants to install one itself.
#define EXAMPLE(name, ...) NX_IMPL_TEST(name, __COUNTER__, example, main_thread __VA_OPT__(, ) __VA_ARGS__)

// An invocable test: an inert test body taking arguments, run only when a driver calls nx::invoke_tests with a matching (decayed) argument signature.
// That is the parametrized / data-driven / generator pattern, and libs/base/nexus/docs/invocable-tests.md has the full mechanism.
// `params` is a parenthesized function parameter list, and the body follows with no trailing ';'.
// Trailing config items compose as with TEST.
//
//   INVOCABLE_TEST("mesh - decimate", (mesh_case const& c), nx::config::seed(3))
//   {
//       CHECK(decimate(c).is_manifold());
//   }
#define NX_IMPL_INVOCABLE_TEST(name, unique_id, params, ...)                                                     \
    static void CC_MACRO_JOIN(_nx_invocable_fn_, unique_id) params;                                              \
    static const bool CC_MACRO_JOIN(_nx_invocable_reg_, unique_id)                                               \
        = (::nx::impl::register_invocable_test(                                                                  \
               name,                                                                                             \
               []()                                                                                              \
               {                                                                                                 \
                   using namespace nx::config;                                                                   \
                   return ::nx::impl::merge_config(__VA_ARGS__);                                                 \
               }(),                                                                                              \
               ::cc::arg_types_of(::cc::signature_of<decltype(&CC_MACRO_JOIN(_nx_invocable_fn_, unique_id))>{}), \
               ::nx::impl::make_test_invoker(&CC_MACRO_JOIN(_nx_invocable_fn_, unique_id)),                      \
               cc::source_location::current()),                                                                  \
           true);                                                                                                \
    static void CC_MACRO_JOIN(_nx_invocable_fn_, unique_id) params

#define INVOCABLE_TEST(name, params, ...) NX_IMPL_INVOCABLE_TEST(name, __COUNTER__, params, __VA_ARGS__)

// A startup hook: its body runs once, before any test listing or scheduling, with full access to the registry via the `nx::setup` handle.
// Used to define aliases — pseudo test-names that expand, under filter matching, into scoped runs of invocable tests.
// Multiple NX_TEST_SETUP blocks compose.
//
//   NX_TEST_SETUP(nx::setup& s)
//   {
//       for (auto const* t : s.invocables_with<sg::context_handle>())
//           s.define_alias(t->name, fragments_for(t));
//   }
#define NX_IMPL_TEST_SETUP(unique_id, ...)                                                                              \
    static void CC_MACRO_JOIN(_nx_setup_fn_, unique_id)(__VA_ARGS__);                                                   \
    static const bool CC_MACRO_JOIN(_nx_setup_reg_, unique_id)                                                          \
        = (::nx::impl::register_setup(&CC_MACRO_JOIN(_nx_setup_fn_, unique_id), cc::source_location::current()), true); \
    static void CC_MACRO_JOIN(_nx_setup_fn_, unique_id)(__VA_ARGS__)

#define NX_TEST_SETUP(...) NX_IMPL_TEST_SETUP(__COUNTER__, __VA_ARGS__)
