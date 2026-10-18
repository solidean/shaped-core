#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

#include <type_traits>

/// Non-owning reference to a callable with signature T, in the spirit of std::function_ref (C++26).
///
/// LIFETIME: the referenced callable must outlive the function_ref, which never owns and never copies.
/// Passing a lambda straight into a parameter is fine — the temporary lives to the end of the full expression.
/// Invoking a stored function_ref after that temporary dies is UB, as it is for any callable that dies first.
///
/// Binds function pointers, lambdas, functors, pointer-to-member functions and pointer-to-member objects.
/// Trivially copyable, no heap allocation, one indirect call, and default-constructible into an invalid state.
///
/// The signature may not be noexcept-qualified or ref-qualified (`R() &&`).
template <class R, class... Args>
struct cc::function_ref<R(Args...)>
{
    // internal storage
private:
    // A function pointer converts to `void*` only conditionally (clang outside Windows refuses), so a function gets its own member.
    union payload
    {
        void* object;
        void (*function)();
    };

    payload _payload = {.object = nullptr};
    cc::function_ptr<R(payload, Args...)> _thunk = nullptr;

    // construction
public:
    /// A default-constructed function_ref is invalid; calling operator() on one is UB.
    function_ref() = default;

    /// Construct from any callable — function pointer, lambda, functor, pointer-to-member function, pointer-to-member object.
    /// Takes every kind of reference, since it must not outlive its argument anyway.
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref>)
    function_ref(F&& f)
    {
        // Future: check if we want this as requires or not
        static_assert(cc::is_invocable_r<R, F&, Args...>, "F must be callable with Args... and return R");

        using Fn = std::remove_reference_t<F>;
        // NOLINTBEGIN
        if constexpr (std::is_function_v<Fn>)
        {
            _payload.function = reinterpret_cast<void (*)()>(&f);
            _thunk = [](payload p, Args... args) -> R
            { return cc::invoke(*reinterpret_cast<Fn*>(p.function), cc::forward<Args>(args)...); };
        }
        else
        {
            _payload.object = const_cast<void*>(static_cast<void const*>(&f));
            _thunk = [](payload p, Args... args) -> R
            { return cc::invoke(*static_cast<Fn*>(p.object), cc::forward<Args>(args)...); };
        }
        // NOLINTEND
    }

    // copy and move (trivial, compiler-generated)
public:
    function_ref(function_ref const&) = default;
    function_ref(function_ref&&) = default;
    function_ref& operator=(function_ref const&) = default;
    function_ref& operator=(function_ref&&) = default;
    ~function_ref() = default;

    // queries
public:
    /// True iff this references a callable; calling operator() when it does not is UB.
    [[nodiscard]] bool is_valid() const { return _thunk != nullptr; }

    [[nodiscard]] explicit operator bool() const { return is_valid(); }

    // invocation
public:
    /// Invoke the referenced callable.
    /// is_valid() must hold, which CC_ASSERT checks wherever assertions are enabled.
    R operator()(Args... args) const
    {
        CC_ASSERT(_thunk != nullptr, "calling invalid function_ref is UB");
        return _thunk(_payload, cc::forward<Args>(args)...);
    }
};
