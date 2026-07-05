#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/traits.hh> // cc::signature
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <nexus/tests/typed_value.hh>

#include <type_traits>
#include <utility> // std::index_sequence

// Generic invocation glue: call any callable with its arguments sourced (by decayed type) from a
// span of typed_value slots. Shared by nx::invoke_tests (INVOCABLE_TEST) and the fuzz engine.

namespace nx::impl
{
template <class F, class R, class... A, std::size_t... I>
typed_value call_with_values(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)>, std::index_sequence<I...>)
{
    CC_ASSERT(inputs.size() == sizeof...(A), "invoked with the wrong number of arguments");
    if constexpr (std::is_void_v<R>)
    {
        cc::invoke(f, inputs[I]->template get<A>()...);
        return {};
    }
    else
    {
        return typed_value::create(cc::invoke(f, inputs[I]->template get<A>()...));
    }
}

// Builds the index sequence from the signature itself. Returns an (invalid) typed_value for void callables.
template <class F, class R, class... A>
typed_value invoke_with_values(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)> s)
{
    return call_with_values(f, inputs, s, std::index_sequence_for<A...>{});
}
} // namespace nx::impl
