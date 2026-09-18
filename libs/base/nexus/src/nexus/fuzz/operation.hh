#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh> // the async invoker's handle type
#include <nexus/fuzz/fwd.hh>
#include <nexus/fuzz/signature.hh>
#include <nexus/fwd.hh>
#include <nexus/tests/typed_value.hh>

#include <type_traits>
#include <typeindex>
#include <typeinfo>

/// A named, typed operation in a fuzz test: a value seed, a mutating/producing call, or an invariant.
///
/// Created via test::add_op / add_value / add_invariant, then configured with the chainable builder
/// (execute_at_least/at_most/once, when(...)). The same object also evaluates directly — eval(...) —
/// which is what emitted regression code uses to replay a recorded program.
struct nx::fuzz::fuzz_operation
{
    /// Upper bound used when no explicit at-most is set ("effectively unbounded").
    static constexpr int unbounded = 1 << 30;

    /// An op returning cc::shared_async<T> is async: the engine awaits it, and T is what reaches the slot.
    /// cc::shared_async<cc::unit> is an async void op.
    template <class F>
    [[nodiscard]] static cc::unique_ptr<fuzz_operation> create(cc::string name, F&& fn)
    {
        using sig_t = cc::signature_of<F>;
        using ret_t = std::remove_cvref_t<decltype(impl::return_of(sig_t{}))>;
        using async_t = impl::async_result_of<ret_t>;
        static_assert(!async_t::is_scheduled,
                      "a fuzz op may not return cc::async_scheduled<T>: it starts before the engine "
                      "awaits it — return the cold cc::shared_async<T> instead");

        auto op = cc::make_unique<fuzz_operation>();
        op->_name = cc::move(name);
        op->_arg_types = cc::arg_types_of(sig_t{});
        op->_arg_is_mutable = cc::arg_is_mutable_of(sig_t{});

        if constexpr (async_t::is_async)
        {
            using value_t = typename async_t::value_type;
            static_assert(std::is_same_v<typename async_t::error_type, cc::async_error>,
                          "an async fuzz op must fail on cc::async_error, the channel the engine reads messages from");
            static_assert(!std::is_const_v<value_t>, "an async fuzz op produces its value, so it cannot be a read-only "
                                                     "cc::shared_async<T const>");
            static_assert(!impl::async_result_of<value_t>::is_async,
                          "an async fuzz op may not produce another cc::shared_async — to keep a pending handle in a "
                          "slot, wrap it in a type of your own");

            op->_is_async = true;
            op->_returns_void = std::is_same_v<value_t, cc::unit>;
            op->_return_type = op->_returns_void ? std::type_index(typeid(void)) : std::type_index(typeid(value_t));
            op->_async_invoker = [fn = cc::forward<F>(fn)](cc::span<typed_value*> in,
                                                           cc::async_scheduler* home) -> cc::shared_async<typed_value>
            {
                auto handle = impl::invoke_raw(fn, in, sig_t{});
                CC_ASSERT(handle != nullptr, "an async fuzz op must return a valid handle");
                if (home != nullptr)
                    (void)handle->try_home_cold(*home); // only a cold body can be placed; a handle already running stays put
                return impl::async_op_glue<value_t>::box(cc::move(handle));
            };
        }
        else
        {
            op->_returns_void = cc::returns_void(sig_t{});
            op->_return_type = cc::return_type_of(sig_t{});
            op->_invoker = [fn = cc::forward<F>(fn)](cc::span<typed_value*> in) -> typed_value
            { return impl::invoke_operation(fn, in, sig_t{}); };
        }
        return op;
    }

    // ---- builder (chainable) ---------------------------------------------------------------------

    // The two bounds must stay ordered, _at_least <= _at_most.
    // The runner keeps scheduling while any op is below its at-least but never runs one past its at-most, so an at-least above the at-most would loop forever.
    // Each setter pulls the other bound along to preserve that.
    fuzz_operation* execute_at_least(int times)
    {
        _at_least = times;
        if (_at_most < times)
            _at_most = times;
        return this;
    }
    fuzz_operation* execute_at_most(int times)
    {
        _at_most = times;
        if (_at_least > times)
            _at_least = times;
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

    /// Adds a precondition guard; multiple guards must all hold.
    /// call_precondition has the three supported arities: nullary external gate, single-arg per-input, exact-arity tuple.
    template <class F>
    fuzz_operation* when(F&& cond)
    {
        using sig_t = cc::signature_of<F>;
        static_assert(!impl::async_result_of<std::remove_cvref_t<decltype(impl::return_of(sig_t{}))>>::is_async,
                      "a precondition must be synchronous: it is evaluated while the next step is chosen");
        _preconditions.push_back([cond = cc::forward<F>(cond)](cc::span<typed_value*> in) -> bool
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
    [[nodiscard]] bool is_async() const { return _is_async; }
    [[nodiscard]] int execute_at_least_times() const { return _at_least; }
    [[nodiscard]] int execute_at_most_times() const { return _at_most; }

    // ---- invocation ------------------------------------------------------------------------------

    /// Calls a synchronous op.
    [[nodiscard]] typed_value invoke(cc::span<typed_value*> inputs) const
    {
        CC_ASSERT(!_is_async, "an async op is called through invoke_async");
        return _invoker(inputs);
    }

    /// Calls an async op, handing back the cold handle that resolves to its boxed value (invalid for a void op).
    /// The handle may still point into `inputs`, so they must outlive it.
    /// A non-null `home` places the op's body there while it is still cold.
    [[nodiscard]] cc::shared_async<typed_value> invoke_async(cc::span<typed_value*> inputs,
                                                             cc::async_scheduler* home = nullptr) const
    {
        CC_ASSERT(_is_async, "a synchronous op is called through invoke");
        return _async_invoker(inputs, home);
    }

    [[nodiscard]] bool check_preconditions(cc::span<typed_value*> inputs) const
    {
        for (auto const& p : _preconditions)
            if (!p(inputs))
                return false;
        return true;
    }

    // ---- direct evaluation (used by regression code) ---------------------------------------------

    /// Boxes the given arguments and invokes the operation.
    /// A typed_value argument is referenced directly, so chained calls share and mutate the same value; anything else is boxed by copy.
    template <class... Args>
    [[nodiscard]] typed_value eval(Args&&... args) const
    {
        cc::vector<typed_value> storage;
        cc::vector<typed_value*> ptrs;
        storage.reserve(sizeof...(Args)); // reserve so &storage.back() stays valid as we fill
        ptrs.reserve(sizeof...(Args));
        (eval_arg(storage, ptrs, cc::forward<Args>(args)), ...);
        return invoke(cc::span<typed_value*>(ptrs));
    }

    /// eval for an async op: the arguments are boxed into the returned coroutine's frame, which calls the op and resolves to its boxed value.
    /// A typed_value argument is referenced directly, so it must outlive the returned handle, as in eval.
    template <class... Args>
    [[nodiscard]] cc::shared_async<typed_value> eval_async(cc::async_scheduler* home, Args&&... args) const
    {
        cc::vector<typed_value> storage;
        cc::vector<typed_value*> external; // null means "the next boxed argument in storage"
        storage.reserve(sizeof...(Args));
        external.reserve(sizeof...(Args));
        (eval_arg_async(storage, external, cc::forward<Args>(args)), ...);
        return eval_async_boxed(cc::move(storage), cc::move(external), home);
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
    // A coroutine, defined in machine.cc; it builds the argument pointers once the storage sits in its frame.
    cc::shared_async<typed_value> eval_async_boxed(cc::vector<typed_value> storage,
                                                   cc::vector<typed_value*> external,
                                                   cc::async_scheduler* home) const;

    template <class A>
    static void eval_arg_async(cc::vector<typed_value>& storage, cc::vector<typed_value*>& external, A&& a)
    {
        using plain = std::remove_cvref_t<A>;
        if constexpr (std::is_same_v<plain, typed_value>)
        {
            external.push_back(const_cast<typed_value*>(&a));
        }
        else
        {
            storage.push_back(typed_value::create(std::forward<A>(a)));
            external.push_back(nullptr);
        }
    }

    template <class A>
    static void eval_arg(cc::vector<typed_value>& storage, cc::vector<typed_value*>& ptrs, A&& a)
    {
        using plain = std::remove_cvref_t<A>;
        if constexpr (std::is_same_v<plain, typed_value>)
        {
            ptrs.push_back(const_cast<typed_value*>(&a)); // reference the caller's value directly
        }
        else
        {
            storage.push_back(typed_value::create(std::forward<A>(a)));
            ptrs.push_back(&storage.back());
        }
    }

    cc::string _name;
    cc::unique_function<typed_value(cc::span<typed_value*>)> _invoker; // a synchronous op
    cc::unique_function<cc::shared_async<typed_value>(cc::span<typed_value*>, cc::async_scheduler*)> _async_invoker; // an async op
    cc::vector<cc::unique_function<bool(cc::span<typed_value*>)>> _preconditions;

    cc::vector<std::type_index> _arg_types;
    cc::vector<bool> _arg_is_mutable;
    std::type_index _return_type = std::type_index(typeid(void));
    bool _returns_void = true;
    bool _is_invariant = false;
    bool _is_async = false;
    int _at_least = 50;
    int _at_most = unbounded;
};
