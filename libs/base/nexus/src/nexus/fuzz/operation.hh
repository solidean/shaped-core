#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <nexus/fuzz/signature.hh>
#include <nexus/fuzz/value.hh>

#include <type_traits>
#include <typeindex>
#include <typeinfo>

namespace nx::fuzz
{
/// A named, typed operation in a fuzz test: a value seed, a mutating/producing call, or an invariant.
///
/// Created via test::add_op / add_value / add_invariant, then configured with the chainable builder
/// (execute_at_least/at_most/once, when(...)). The same object also evaluates directly — eval(...) —
/// which is what emitted regression code uses to replay a recorded program.
struct fuzz_operation
{
    /// Upper bound used when no explicit at-most is set ("effectively unbounded").
    static constexpr int unbounded = 1 << 30;

    template <class F>
    [[nodiscard]] static cc::unique_ptr<fuzz_operation> create(cc::string name, F&& fn)
    {
        using sig_t = impl::signature_of<F>;
        auto op = cc::make_unique<fuzz_operation>();
        op->_name = cc::move(name);
        op->_arg_types = impl::arg_types_of(sig_t{});
        op->_arg_is_mutable = impl::arg_is_mutable_of(sig_t{});
        op->_returns_void = impl::returns_void(sig_t{});
        op->_return_type = impl::return_type_of(sig_t{});
        op->_invoker = [fn = cc::forward<F>(fn)](cc::span<fuzz_value*> in) -> fuzz_value
        { return impl::invoke_operation(fn, in, sig_t{}); };
        return op;
    }

    // ---- builder (chainable) ---------------------------------------------------------------------

    fuzz_operation* execute_at_least(int times)
    {
        _at_least = times;
        return this;
    }
    fuzz_operation* execute_at_most(int times)
    {
        _at_most = times;
        return this;
    }
    fuzz_operation* execute_once() { return execute_at_least(1)->execute_at_most(1); }

    /// Invariants are never scheduled on their own; the machine checks them after any operation that
    /// produces or mutates a value of their argument type.
    fuzz_operation* mark_as_invariant()
    {
        _is_invariant = true;
        _at_least = 0;
        _at_most = 0;
        return this;
    }

    /// Adds a precondition guard. Multiple guards all must hold. See call_precondition for the three
    /// supported arities (nullary external gate / single-arg per-input / exact-arity tuple).
    template <class F>
    fuzz_operation* when(F&& cond)
    {
        using sig_t = impl::signature_of<F>;
        _preconditions.push_back([cond = cc::forward<F>(cond)](cc::span<fuzz_value*> in) -> bool
                                 { return impl::invoke_precondition(cond, in, sig_t{}); });
        return this;
    }

    // ---- metadata --------------------------------------------------------------------------------

    [[nodiscard]] cc::string const& name() const { return _name; }
    [[nodiscard]] cc::span<std::type_index const> arg_types() const { return _arg_types; }
    [[nodiscard]] cc::span<bool const> arg_is_mutable() const { return _arg_is_mutable; }
    [[nodiscard]] std::type_index return_type() const { return _return_type; }
    [[nodiscard]] bool returns_void() const { return _returns_void; }
    [[nodiscard]] bool is_invariant() const { return _is_invariant; }
    [[nodiscard]] int execute_at_least_times() const { return _at_least; }
    [[nodiscard]] int execute_at_most_times() const { return _at_most; }

    // ---- invocation ------------------------------------------------------------------------------

    [[nodiscard]] fuzz_value invoke(cc::span<fuzz_value*> inputs) const { return _invoker(inputs); }

    [[nodiscard]] bool check_preconditions(cc::span<fuzz_value*> inputs) const
    {
        for (auto const& p : _preconditions)
            if (!p(inputs))
                return false;
        return true;
    }

    // ---- direct evaluation (used by regression code) ---------------------------------------------

    /// Boxes the given arguments and invokes the operation. A fuzz_value argument is referenced
    /// directly (so chained calls share and mutate the same value); anything else is boxed by copy.
    template <class... Args>
    [[nodiscard]] fuzz_value eval(Args&&... args) const
    {
        cc::vector<fuzz_value> storage;
        cc::vector<fuzz_value*> ptrs;
        storage.reserve(sizeof...(Args)); // reserve so &storage.back() stays valid as we fill
        ptrs.reserve(sizeof...(Args));
        (eval_arg(storage, ptrs, cc::forward<Args>(args)), ...);
        return _invoker(cc::span<fuzz_value*>(ptrs));
    }

    template <class T, class... Args>
    [[nodiscard]] T eval_to(Args&&... args) const
    {
        return eval(std::forward<Args>(args)...).template get<T>();
    }

    template <class... Args>
    [[nodiscard]] bool eval_bool(Args&&... args) const
    {
        return eval(std::forward<Args>(args)...).get_bool();
    }

private:
    template <class A>
    static void eval_arg(cc::vector<fuzz_value>& storage, cc::vector<fuzz_value*>& ptrs, A&& a)
    {
        using plain = std::remove_cvref_t<A>;
        if constexpr (std::is_same_v<plain, fuzz_value>)
        {
            ptrs.push_back(const_cast<fuzz_value*>(&a)); // reference the caller's value directly
        }
        else
        {
            storage.push_back(fuzz_value::create(std::forward<A>(a)));
            ptrs.push_back(&storage.back());
        }
    }

    cc::string _name;
    cc::unique_function<fuzz_value(cc::span<fuzz_value*>)> _invoker;
    cc::vector<cc::unique_function<bool(cc::span<fuzz_value*>)>> _preconditions;

    cc::vector<std::type_index> _arg_types;
    cc::vector<bool> _arg_is_mutable;
    std::type_index _return_type = std::type_index(typeid(void));
    bool _returns_void = true;
    bool _is_invariant = false;
    int _at_least = 50;
    int _at_most = unbounded;
};
} // namespace nx::fuzz
