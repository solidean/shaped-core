#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/fuzz/value.hh>

#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility> // std::index_sequence: compile-time arg packs (no cc:: equivalent)

// Compile-time signature deduction for fuzz operations / preconditions.
//
// Any callable (free function, lambda/functor, member-function/data pointer) is reduced to a
// signature<R(A...)>. From that we extract, once at setup time: the decayed argument types, which
// arguments are mutable (non-const lvalue references), and the return type. Invocation then erases
// to a uniform fuzz_value(cc::span<fuzz_value*>) call.

namespace nx::fuzz::impl
{
template <class R, class... A>
struct signature
{
};

// member function pointers (this also covers a lambda's operator())
template <class T, class R, class... A>
signature<R(A...)> deduce_member(R (T::*)(A...));
template <class T, class R, class... A>
signature<R(A...)> deduce_member(R (T::*)(A...) const);
template <class T, class R, class... A>
signature<R(A...)> deduce_member(R (T::*)(A...) noexcept);
template <class T, class R, class... A>
signature<R(A...)> deduce_member(R (T::*)(A...) const noexcept);

// free function
template <class R, class... A>
signature<R(A...)> deduce_signature(R (*)(A...));

// lambda / functor: deduce from its operator()
template <class F>
auto deduce_signature(F&&) -> decltype(deduce_member(&std::decay_t<F>::operator()));

template <class F>
using signature_of = decltype(deduce_signature(std::declval<F>()));

// ---- metadata extraction -------------------------------------------------------------------------

template <class R, class... A>
cc::vector<std::type_index> arg_types_of(signature<R(A...)>)
{
    cc::vector<std::type_index> r;
    (r.push_back(std::type_index(typeid(std::decay_t<A>))), ...);
    return r;
}

template <class R, class... A>
cc::vector<bool> arg_is_mutable_of(signature<R(A...)>)
{
    cc::vector<bool> r;
    (r.push_back(std::is_lvalue_reference_v<A> && !std::is_const_v<std::remove_reference_t<A>>), ...);
    return r;
}

template <class R, class... A>
bool returns_void(signature<R(A...)>)
{
    return std::is_void_v<R>;
}

template <class R, class... A>
std::type_index return_type_of(signature<R(A...)>)
{
    if constexpr (std::is_void_v<R>)
        return std::type_index(typeid(void));
    else
        return std::type_index(typeid(std::decay_t<R>));
}

// ---- invocation ----------------------------------------------------------------------------------

template <class F, class R, class... A, std::size_t... I>
fuzz_value call_operation(F const& f, cc::span<fuzz_value*> inputs, signature<R(A...)>, std::index_sequence<I...>)
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
fuzz_value invoke_operation(F const& f, cc::span<fuzz_value*> inputs, signature<R(A...)> s)
{
    return call_operation(f, inputs, s, std::index_sequence_for<A...>{});
}

// Preconditions may be nullary (external gate), single-argument (must hold for every matching input),
// or exact-arity (full tuple over the operation's inputs).
template <class F, class R, class... A, std::size_t... I>
bool call_precondition(F const& f, cc::span<fuzz_value*> inputs, signature<R(A...)>, std::index_sequence<I...>)
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
bool invoke_precondition(F const& f, cc::span<fuzz_value*> inputs, signature<R(A...)> s)
{
    return call_precondition(f, inputs, s, std::index_sequence_for<A...>{});
}
} // namespace nx::fuzz::impl
