#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>

#include <concepts>
#include <type_traits>
#include <typeindex>
#include <typeinfo>

namespace cc
{
template <class R, class... A>
struct signature;

/// A range that knows its size and can be subscripted by an index — what sorting, shuffling and picking take.
/// Subscripting must accept an isize, and size() must convert to one.
/// Says nothing about contiguity or iterators: a strided_span models this, and so does a hand-written proxy.
template <class R>
concept indexed_range = requires(R& r, isize i) {
    { r.size() } -> std::convertible_to<isize>;
    r[i];
};
} // namespace cc

// Compile-time signature reflection for arbitrary callables.
//
// Any callable — a free function, a lambda or functor, a member-function pointer — is reduced to a signature<R(A...)>.
// From that we extract the decayed argument types, which arguments are mutable (non-const lvalue references), and the return type.
// This is the generic machinery; callers wrap it in their own invocation glue.

template <class R, class... A>
struct cc::signature
{
};

namespace cc
{

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
} // namespace cc
