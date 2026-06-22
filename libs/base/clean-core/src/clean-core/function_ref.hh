#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/utility.hh>

#include <type_traits>

/// Non-owning reference to a callable object with signature T
///
/// Similar to std::function_ref (C++26), provides a lightweight, trivially copyable view
/// of any callable object without owning it.
///
/// IMPORTANT LIFETIME RULE:
///   function_ref never owns. Any referenced callable object must outlive the function_ref.
///   Binding to temporaries or references with shorter lifetime will result in UB.
///
/// Supports implicit construction from:
///   - function pointers
///   - lambdas / functors
///   - pointer-to-member functions
///   - pointer-to-member objects
///
/// Usage example:
///   void process(cc::function_ref<int(int)> f) {
///       auto const result = f(42);
///   }
///
///   auto const add_one = [](int x) { return x + 1; };
///   process(add_one);
///   process([](int x) { return x * 2; });
///   process(+[](int x) { return x - 1; });  // unary + converts to function pointer
///
/// Properties:
///   - Trivially copyable (no destructor, no heap allocations)
///   - Zero overhead abstraction (single indirect call)
///   - Default constructible (creates invalid/null state)
///
/// Limitations:
///   - Does not support noexcept qualification in signature
///   - Does not support ref-qualified signatures (e.g., R() &&)
template <class R, class... Args>
struct cc::function_ref<R(Args...)>
{
    // internal storage
private:
    void* _payload = nullptr;
    cc::function_ptr<R(void*, Args...)> _thunk = nullptr;

    // construction
public:
    /// default constructor creates an invalid/null function_ref
    /// calling operator() on a default-constructed function_ref is UB
    function_ref() = default;

    /// construct from any callable
    /// accepts function pointers, lambdas, functors, pointer-to-member-function, pointer-to-member-object
    /// accepts all kinds of references because it must not outlive its arg anyways
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref>)
    function_ref(F&& f) : _payload(&f)
    {
        // Future: check if we want this as requires or not
        static_assert(cc::is_invocable_r<R, F&, Args...>, "F must be callable with Args... and return R");

        using Fn = std::remove_reference_t<F>;
        // NOLINTBEGIN
        _thunk = [](void* p, Args... args) -> R { return cc::invoke(*static_cast<Fn*>(p), cc::forward<Args>(args)...); };
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
    /// returns true if this function_ref references a valid callable
    /// calling operator() when !is_valid() is UB
    [[nodiscard]] bool is_valid() const { return _thunk != nullptr; }

    /// returns true if this function_ref references a valid callable
    [[nodiscard]] explicit operator bool() const { return is_valid(); }

    // invocation
public:
    /// invoke the referenced callable with the given arguments
    /// precondition: is_valid() must be true (checked via CC_ASSERT in debug)
    R operator()(Args... args) const
    {
        CC_ASSERT(_thunk != nullptr, "calling invalid function_ref is UB");
        return _thunk(_payload, cc::forward<Args>(args)...);
    }
};
