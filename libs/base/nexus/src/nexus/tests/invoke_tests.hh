#pragma once

#include <clean-core/common/traits.hh> // cc::arg_types_of / cc::signature
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/typed_value.hh>

#include <typeindex>

namespace nx
{
struct test_declaration;

/// Outcome of an nx::invoke_tests call.
struct invocation_result
{
    int matched = 0;  ///< invocable tests whose signature matched (before -c / name scoping)
    int executed = 0; ///< instances actually run after scoping
};

namespace impl
{
invocation_result invoke_tests_impl(cc::string_view name,
                                    cc::span<std::type_index const> signature,
                                    cc::span<nx::typed_value*> values);

// Element-wise equality of two decayed argument-type lists (the invoke_tests / alias join key).
bool signatures_equal(cc::span<std::type_index const> a, cc::span<std::type_index const> b);
} // namespace impl

/// Runs every INVOCABLE_TEST whose *decayed* argument signature matches `Args...`, passing `args...`.
/// Call from inside an ordinary (driver) test body. Each matched test runs as an addressable child under
/// the section segment `name` (its own name and sections nest below), and the expensive setup around the
/// call happens once — the driver body itself is not re-run per child.
///
/// Usually the template argument is left to deduce (`nx::invoke_tests("case", load(f))`); the key is the
/// decayed type list. `name` is authored, never derived from a value, so output/addresses stay stable.
/// Arguments are boxed by (decayed) value; prefer cheap-to-copy / handle types, or pass large data behind
/// a handle/pointer.
template <class... Args>
invocation_result invoke_tests(cc::string_view name, Args... args)
{
    static_assert(sizeof...(Args) >= 1, "nx::invoke_tests needs at least one argument (the join key)");

    auto const signature = cc::arg_types_of(cc::signature<void(Args...)>{});

    cc::vector<typed_value> boxes;
    boxes.reserve(sizeof...(Args)); // reserve so element addresses stay valid as we fill
    (boxes.push_back(typed_value::create(cc::move(args))), ...);

    cc::vector<typed_value*> ptrs;
    ptrs.reserve(boxes.size());
    for (auto& b : boxes)
        ptrs.push_back(&b);

    return impl::invoke_tests_impl(name, signature, ptrs);
}
} // namespace nx
