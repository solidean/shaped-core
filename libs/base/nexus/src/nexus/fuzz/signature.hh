#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <nexus/tests/typed_invoke.hh>
#include <nexus/tests/typed_value.hh>

#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility> // std::index_sequence: compile-time arg packs (no cc:: equivalent)

// Invocation glue for fuzz operations / preconditions.
//
// The generic callable reflection (cc::signature_of, arg_types_of, ...) lives in clean-core's common/traits.hh.
// The uniform "invoke a callable from a span of typed_value slots" glue lives in nexus/tests/typed_invoke.hh.
// This file adds the fuzz-specific precondition-arity dispatch, and how an async op is recognised from its return type.

namespace nx::fuzz::impl
{
// ---- invocation ----------------------------------------------------------------------------------

// Thin alias onto the shared glue (nx::impl::invoke_with_values). Kept so operation.hh reads naturally.
template <class F, class R, class... A>
typed_value invoke_operation(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)> s)
{
    return ::nx::impl::invoke_with_values(f, inputs, s);
}

// Preconditions may be nullary (external gate), single-argument (must hold for every matching input),
// or exact-arity (full tuple over the operation's inputs).
template <class F, class R, class... A, std::size_t... I>
bool call_precondition(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)>, std::index_sequence<I...>)
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
bool invoke_precondition(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)> s)
{
    return call_precondition(f, inputs, s, std::index_sequence_for<A...>{});
}

// ---- async operations ----------------------------------------------------------------------------

template <class R, class... A>
R return_of(cc::signature<R(A...)>); // unevaluated only: the signature's return type

/// What an op's return type says about it: a cc::shared_async<T> makes it an async op producing T.
template <class R>
struct async_result_of
{
    static constexpr bool is_async = false;
    static constexpr bool is_scheduled = false;
};
template <class T, class E>
struct async_result_of<cc::shared_async<T, E>>
{
    static constexpr bool is_async = true;
    static constexpr bool is_scheduled = false;
    using value_type = T;
    using error_type = E;
};
template <class T, class E>
struct async_result_of<cc::async_scheduled<T, E>>
{
    static constexpr bool is_async = false;
    static constexpr bool is_scheduled = true;
};

/// Turns an async op's handle into the handle the engine awaits: the op's value, boxed.
/// Defined in nexus/fuzz/async.hh — registering an async op needs that header, which is where the coroutine machinery lives.
template <class T>
struct async_op_glue;

// Calls an op and hands back its result unboxed, which for an async op is its cc::shared_async.
template <class F, class R, class... A, std::size_t... I>
R call_raw(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)>, std::index_sequence<I...>)
{
    CC_ASSERT(inputs.size() == sizeof...(A), "invoked with the wrong number of arguments");
    return cc::invoke(f, inputs[I]->template get<A>()...);
}

template <class F, class R, class... A>
R invoke_raw(F const& f, cc::span<typed_value*> inputs, cc::signature<R(A...)> s)
{
    return call_raw(f, inputs, s, std::index_sequence_for<A...>{});
}
} // namespace nx::fuzz::impl
