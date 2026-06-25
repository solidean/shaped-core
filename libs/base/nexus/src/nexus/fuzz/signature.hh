#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/fuzz/value.hh>

#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility> // std::index_sequence: compile-time arg packs (no cc:: equivalent)

// Invocation glue for fuzz operations / preconditions.
//
// The generic callable reflection (cc::signature_of, arg_types_of, ...) lives in clean-core's
// common/traits.hh. Here we add the fuzz-specific layer: erasing any callable to a uniform
// fuzz_value(cc::span<fuzz_value*>) call, and the precondition-arity dispatch.

namespace nx::fuzz::impl
{
// ---- invocation ----------------------------------------------------------------------------------

template <class F, class R, class... A, std::size_t... I>
fuzz_value call_operation(F const& f, cc::span<fuzz_value*> inputs, cc::signature<R(A...)>, std::index_sequence<I...>)
{
    CC_ASSERT(inputs.size() == sizeof...(A), "operation invoked with the wrong number of arguments");
    if constexpr (std::is_void_v<R>)
    {
        cc::invoke(f, inputs[I]->template get<A>()...);
        return {};
    }
    else
    {
        return fuzz_value::create(cc::invoke(f, inputs[I]->template get<A>()...));
    }
}

// Convenience wrappers that build the index sequence from the signature itself.
template <class F, class R, class... A>
fuzz_value invoke_operation(F const& f, cc::span<fuzz_value*> inputs, cc::signature<R(A...)> s)
{
    return call_operation(f, inputs, s, std::index_sequence_for<A...>{});
}

// Preconditions may be nullary (external gate), single-argument (must hold for every matching input),
// or exact-arity (full tuple over the operation's inputs).
template <class F, class R, class... A, std::size_t... I>
bool call_precondition(F const& f, cc::span<fuzz_value*> inputs, cc::signature<R(A...)>, std::index_sequence<I...>)
{
    static_assert(std::is_same_v<R, bool>, "preconditions must return bool");

    if constexpr (sizeof...(A) == 0)
    {
        return cc::invoke(f);
    }
    else if constexpr (sizeof...(A) == 1)
    {
        // applied to every input whose type matches the single parameter
        auto const want = (std::type_index(typeid(std::decay_t<A>)), ...);
        for (auto* in : inputs)
            if (in->type() == want)
                if (!cc::invoke(f, in->template get<A>()...))
                    return false;
        return true;
    }
    else
    {
        CC_ASSERT(inputs.size() == sizeof...(A), "exact-arity precondition argument count mismatch");
        return cc::invoke(f, inputs[I]->template get<A>()...);
    }
}

template <class F, class R, class... A>
bool invoke_precondition(F const& f, cc::span<fuzz_value*> inputs, cc::signature<R(A...)> s)
{
    return call_precondition(f, inputs, s, std::index_sequence_for<A...>{});
}
} // namespace nx::fuzz::impl
