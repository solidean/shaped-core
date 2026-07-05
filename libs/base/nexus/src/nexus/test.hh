#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/common/traits.hh> // cc::signature_of / cc::arg_types_of
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/source_location.hh>
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

// Registers an invocable (inert) test. `signature` is the decayed argument-type join key and `fn` runs
// the body with args sourced from typed_value slots. Non-template so test.hh stays light.
void register_invocable_test(char const* name,
                             config::cfg test_config,
                             cc::vector<std::type_index> signature,
                             cc::unique_function<void(cc::span<nx::typed_value*>)> fn,
                             cc::source_location loc);

// Wraps a `void(A...)` test body into the type-erased invoker stored in the registry: it unpacks the
// typed_value slots back into the concrete argument types and calls the body. Mutable lvalue-reference
// parameters are rejected — matching decays the signature and the boxed args are shared read-only inputs,
// so a `T&` parameter would silently share/mutate one box across instances. Use `T` or `T const&`.
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

// A guide benchmark: a test in the guide_benchmark bucket that reports metrics via nx::guide (see guide.hh).
// Swept only via --guide-benchmarks (or named explicitly), never in a normal run. Extra config items compose
// as with TEST, e.g. GUIDE_BENCHMARK("name", seed(42)).
#define GUIDE_BENCHMARK(name, ...) NX_IMPL_TEST(name, __COUNTER__, guide_benchmark __VA_OPT__(, ) __VA_ARGS__)

// An invocable test: an inert test body taking arguments, run only when a driver calls nx::invoke_tests
// with a matching (decayed) argument signature (the parametrized / data-driven / generator pattern).
// `params` is a parenthesized function parameter list; the body follows with no trailing ';'. Trailing
// config items compose as with TEST.
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
