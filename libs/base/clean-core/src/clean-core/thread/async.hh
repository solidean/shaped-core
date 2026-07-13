#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async_node.hh>

#include <memory>      // std::shared_ptr, std::enable_shared_from_this
#include <type_traits> // std::decay_t

// cc::async<T> — a low-overhead value/dataflow async for compute-heavy dependency graphs.
//
// The mental model is values and dataflow transformations, not futures/promises or callback chains. An
// async<T> is an eventual result<T, async_error> produced by a compute frame: a callable/state-machine
// polled through an async_context. Polling never blocks a thread; a frame that cannot yet progress parks on
// its not-ready dependencies and is woken when they complete.
//
//   auto a = cc::make_async_scheduled<int>([](cc::async_context&) { return 40; });
//   auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);   // b depends on a; f gets a plain int
//   int v = cc::async_blocking_get(b);   // drives the graph on this thread -> 42
//
// Handles:
//   shared_async<T> = std::shared_ptr<async<T>>  — the normal, composable, many-dependent handle. async<T>
//     itself is non-copyable and immovable; you copy the shared_ptr, never the node.
//
// Design notes worth stating up front:
//   * pending-dependency lists hold only NOT-ready deps, purely for scheduling/wakeup.
//   * compute-frame captures are what retain observed shared_async dependencies (not the pending list).
//   * subscriptions/continuations are late-installed wakeup hints, added only when a frame must park.

namespace cc
{
template <class T>
using shared_async = std::shared_ptr<async<T>>;

// ============================================================================
// async_result<T> — what a compute frame returns from one step
// ============================================================================

namespace impl
{
// local declval (clean-core does not bless <utility>): produces a value of type U in unevaluated context
template <class U>
U async_declval() noexcept;
} // namespace impl

/// The outcome of one compute step: a value, a failure, a request to wait for dependencies, or a yield.
/// Implicitly constructible from a plain T (the common "return the value" success path) and from the tiny
/// tag types async_context hands out, so a frame can simply `return value;` or `return actx.wait_...();`.
/// Move-only (carries a move-only async_error). Use T = cc::unit to model an async<void>.
template <class T>
struct async_result
{
    // success from a plain value — intentionally implicit so `return value;` works inside a frame
    async_result(T v) : _status(async_status::value) { _value.emplace_value(cc::move(v)); }

    // from async_context helpers (all intentionally implicit)
    template <class V>
    async_result(async_success_tag<V> s) : _status(async_status::value)
    {
        _value.emplace_value(cc::move(s.value));
    }
    async_result(async_waiting_tag) : _status(async_status::waiting) {}
    async_result(async_yield_tag) : _status(async_status::yield) {}
    async_result(async_error_result_tag e) : _status(async_status::error), _error(cc::move(e.error)) {}

    [[nodiscard]] async_status status() const { return _status; }

    // consumed by the node once a step finishes; valid for the matching status only
    [[nodiscard]] T take_value() && { return cc::move(_value.value()); }
    [[nodiscard]] async_error take_error() && { return cc::move(_error); }

    async_result(async_result&&) noexcept = default;
    async_result& operator=(async_result&&) noexcept = default;
    async_result(async_result const&) = delete;
    async_result& operator=(async_result const&) = delete;

private:
    async_status _status;
    cc::optional<T> _value;
    async_error _error;
};

// ============================================================================
// typed node: the value + compute-frame layer under async<T>
// ============================================================================

namespace impl
{
/// Holds the typed value and the type-erased compute frame; implements the two virtuals the base poll loop
/// needs. async<T> adds only its handle-specific surface on top.
template <class T>
struct async_typed_node : cc::async_node_base
{
    /// Install the compute frame. F is called as `async_result<T>(async_context&)`.
    template <class F>
    void set_frame(F&& f)
    {
        _frame = cc::unique_function<cc::async_result<T>(cc::async_context&)>(cc::forward<F>(f));
    }

    /// Pointer to the produced value; null unless ready with a value. Stable while the node is alive.
    [[nodiscard]] T const* value_ptr() const { return this->has_value() ? &_value.value() : nullptr; }
    [[nodiscard]] T* value_ptr() { return this->has_value() ? &_value.value() : nullptr; }

    async_step_status poll_compute_step(cc::async_context& ctx) override
    {
        CC_ASSERT(_frame.is_valid(), "polled a node without a compute frame");
        auto r = _frame(ctx);
        switch (r.status())
        {
        case async_status::value:
            _value.emplace_value(cc::move(r).take_value());
            return async_step_status::produced_value;
        case async_status::error:
            this->set_error(cc::move(r).take_error());
            return async_step_status::produced_error;
        case async_status::waiting:
            return async_step_status::waiting;
        case async_status::yield:
            return async_step_status::yield;
        }
        CC_UNREACHABLE("invalid async_status");
    }

    void destroy_frame() override { _frame = {}; }

    // teardown order matters: stop referencing deps, then drop our frame (releasing its captures)
    ~async_typed_node() override
    {
        this->unsubscribe_all();
        _frame = {};
    }

protected:
    cc::optional<T> _value;
    cc::unique_function<cc::async_result<T>(cc::async_context&)> _frame;
};
} // namespace impl

// ============================================================================
// async<T> — the normal shared handle
// ============================================================================

/// The normal composable async handle. Always used through shared_async<T> = std::shared_ptr<async<T>>;
/// the node is non-copyable and immovable. Create with cc::make_async_lazy / cc::make_async_scheduled (the
/// variadic dependency form handles single- and multi-dependency transforms), drive with
/// cc::async_blocking_get. Shared ownership comes from async_node_base.
template <class T>
struct async : impl::async_typed_node<T>
{
    // zero-copy access
public:
    /// Pointer to the stored value, or null unless ready with a value. Non-owning — valid while the node is
    /// alive (you keep it alive through the shared_async handle, not through this pointer).
    [[nodiscard]] T const* try_value() const { return this->value_ptr(); }

    /// Pointer to the failure-channel value, or null unless ready with an error. Non-owning — valid while the
    /// node is alive.
    [[nodiscard]] async_error const* try_error() const { return this->has_error() ? &this->base_error() : nullptr; }

    // manual / promise-style completion (for externally produced values)
public:
    /// Mark this node as awaiting external completion (no compute frame; never run inline). See make_async_manual.
    void set_manual() { this->mark_external_pending(); }

    /// Complete externally with a value; wakes any parked dependents. Call at most once.
    void push_value(T v)
    {
        this->_value.emplace_value(cc::move(v));
        this->mark_ready_and_notify();
    }

    /// Complete externally with an error; wakes any parked dependents. Call at most once.
    void push_error(async_error e)
    {
        this->set_error(cc::move(e));
        this->mark_ready_and_notify();
    }
};

// ============================================================================
// async_context — handed to every compute step
// ============================================================================

/// The interface a compute frame uses to read dependencies and report its outcome. Not owned by the frame;
/// valid only for the duration of a single step.
struct async_context
{
    async_node_base* current = nullptr;
    async_scheduler* scheduler = nullptr;

    // result helpers (each returns a tag that converts to async_result<T>)
public:
    [[nodiscard]] async_waiting_tag wait_for_dependencies() const { return {}; }
    [[nodiscard]] async_yield_tag yield() const { return {}; }
    [[nodiscard]] async_error_result_tag error(async_error e) const { return {cc::move(e)}; }
    [[nodiscard]] async_error_result_tag error(cc::any_error e) const { return {async_error::make_error(cc::move(e))}; }

    template <class V>
    [[nodiscard]] async_success_tag<std::decay_t<V>> success(V&& v) const
    {
        return {cc::forward<V>(v)};
    }

    // dependencies
public:
    /// Require an existing async as a dependency. Returns true if it is already ready (read its value now);
    /// otherwise records it as a pending dependency and returns false — the frame should then return
    /// wait_for_dependencies(). No subscription happens here; that is installed late, only if this node parks.
    template <class T>
    bool require(shared_async<T> const& dep) const
    {
        CC_ASSERT(dep != nullptr, "cannot require a null async");
        if (dep->is_ready())
            return true;
        current->add_pending_dependency(dep.get());
        if (dep->is_cold())
            dep->schedule(); // a required-but-cold dependency must be made runnable, else nobody drives it
        return false;
    }
};

// ============================================================================
// dependency-argument plumbing (for the variadic make_async_* forms)
// ============================================================================

namespace impl
{
// sentinel result type: "deduce T from the function applied to the unwrapped dependency arguments"
struct async_deduce_result
{
};
template <class T>
inline constexpr bool async_is_deduce = std::is_same_v<T, async_deduce_result>;

// A dependency argument is a shared_async<U>: awaited before f runs, then unwrapped to the stored U. Plain
// pass-through values are not supported yet — capture them in the closure.

template <class U>
bool async_require_arg(async_context& ctx, shared_async<U> const& dep)
{
    return ctx.require(dep);
}

template <class U>
void async_collect_arg_error(cc::optional<async_error>& out, shared_async<U> const& dep)
{
    if (!out.has_value() && dep->has_error())
        out.emplace_value(dep->propagate_error());
}

// returns a reference into the dependency node's stored value (stable while the node is alive)
template <class U>
U const& async_unwrap_arg(shared_async<U> const& dep)
{
    return *dep->value_ptr();
}

// invoke the user function, with the leading async_context& if it takes one, else without it
template <class F, class... Args>
decltype(auto) async_invoke_frame_fn(F& f, async_context& ctx, Args&&... args)
{
    if constexpr (std::is_invocable_v<F&, async_context&, Args...>)
        return f(ctx, cc::forward<Args>(args)...);
    else
        return f(cc::forward<Args>(args)...);
}

// value type a frame produces: async_result<S> / success(S) -> S; any other return value R -> R.
// (async_error_result_tag and the wait/yield tags carry no value type, so a frame that only ever returns one
// of those must give T explicitly or annotate its return type — as it would need to anyway.)
template <class R>
struct async_frame_value
{
    using type = R;
};
template <class S>
struct async_frame_value<async_result<S>>
{
    using type = S;
};
template <class S>
struct async_frame_value<async_success_tag<S>>
{
    using type = S;
};

// deduced result type of a frame f applied to the unwrapped dependency arguments (with or without ctx)
template <class F, class... Deps>
using async_deduced_frame_result_t = typename async_frame_value<std::remove_cvref_t<decltype(async_invoke_frame_fn(
    async_declval<std::decay_t<F>&>(),
    async_declval<async_context&>(),
    async_unwrap_arg(async_declval<std::decay_t<Deps> const&>())...))>>::type;

// The one compute-frame wrapper behind every make_async_* / map form. Requires all deps,
// short-circuits on the first error, then calls f — passing async_context& only if f wants it. A raw
// state-machine frame is just the zero-dependency case where f takes async_context& and returns async_result.
template <class R, class F, class... Deps>
auto async_make_frame(F&& f, Deps&&... deps)
{
    return [fn = cc::forward<F>(f), ... ds = cc::forward<Deps>(deps)](async_context& ctx) mutable -> async_result<R>
    {
        bool all_ready = true;
        ((all_ready = async_require_arg(ctx, ds) && all_ready), ...); // require EVERY dep (registers pending)
        if (!all_ready)
            return ctx.wait_for_dependencies();

        cc::optional<async_error> err;
        (async_collect_arg_error(err, ds), ...); // the first errored dependency short-circuits f
        if (err.has_value())
            return ctx.error(cc::move(err.value()));

        return async_invoke_frame_fn(fn, ctx, async_unwrap_arg(ds)...);
    };
}

// shared factory for make_async_*: build async<R> and install the wrapped frame
template <class T, class F, class... Deps>
auto async_make_node(F&& f, Deps&&... deps)
{
    using result_t = std::conditional_t<async_is_deduce<T>, async_deduced_frame_result_t<F, Deps...>, T>;
    static_assert(!std::is_void_v<result_t>, "the frame must return a value (wrap void as cc::unit)");

    auto node = std::make_shared<async<result_t>>();
    node->set_frame(async_make_frame<result_t>(cc::forward<F>(f), cc::forward<Deps>(deps)...));
    return node;
}
} // namespace impl

// ============================================================================
// creation
// ============================================================================

/// Create a cold (lazy) async — it runs only once scheduled (required by another async, or driven by
/// async_blocking_get). The compute frame is `f`, optionally followed by dependency arguments:
///
///   * each dependency is a shared_async<U>; it is awaited and unwrapped to the stored U before f runs.
///     Errors short-circuit: if any dependency failed, f is skipped and the first error propagates.
///   * f is called with the unwrapped dependency values. It may take a leading async_context& (for a raw
///     state-machine that manages its own dependencies) or omit it entirely — both are wrapped as needed.
///   * T defaults to the deduced result of f (async_result<S> counts as S), or may be given explicitly.
///
///   auto a = cc::make_async_scheduled<int>([](cc::async_context&) { return 40; });  // raw frame
///   auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);                    // depends on a; f gets int
///   auto c = cc::make_async_lazy([] { return 7; });                                  // no deps, no context
template <class T = impl::async_deduce_result, class F, class... Deps>
[[nodiscard]] auto make_async_lazy(F&& f, Deps&&... deps)
{
    return impl::async_make_node<T>(cc::forward<F>(f), cc::forward<Deps>(deps)...);
}

/// Like make_async_lazy, but eager: schedules the node immediately if a worker scope is active on this thread
/// (otherwise it stays cold and is scheduled when first required/driven). Same forms as make_async_lazy.
template <class T = impl::async_deduce_result, class F, class... Deps>
[[nodiscard]] auto make_async_scheduled(F&& f, Deps&&... deps)
{
    auto node = impl::async_make_node<T>(cc::forward<F>(f), cc::forward<Deps>(deps)...);
    if (async_scheduler::current_or_null() != nullptr)
        node->schedule();
    return node;
}

/// Create an async completed externally via async<T>::push_value / push_error (a promise-style node). A
/// dependent that requires it parks until it is pushed.
template <class T>
[[nodiscard]] shared_async<T> make_async_manual()
{
    auto node = std::make_shared<async<T>>();
    node->set_manual();
    return node;
}

// ============================================================================
// driving — BLOCKING (top-level / tests only)
// ============================================================================

/// Drive `root` to completion on the calling thread and return its outcome. This BLOCKS the calling thread:
/// with the inline scheduler it pumps work here, and against a future concurrent scheduler it would busy-spin
/// waiting — so it is a top-level / test convenience, never something to call from inside a frame (park on a
/// dependency instead). Asserts if the graph cannot complete (e.g. blocked on an external/manual node that is
/// never pushed — drive those with an explicit inline_scheduler + async_worker_scope).
template <class T>
[[nodiscard]] cc::result<T, async_error> try_async_blocking_get(shared_async<T> const& root)
{
    CC_ASSERT(root != nullptr, "cannot drive a null async");

    inline_scheduler scheduler;
    async_worker_scope scope(scheduler);

    root->schedule();
    scheduler.run_until([&] { return root->is_ready(); });

    CC_ASSERT(root->is_ready(), "async graph could not complete (blocked on external work?)");

    if (root->has_error())
        return cc::error(root->propagate_error());
    return *root->value_ptr(); // copy out
}

/// Drive `root` to completion and return its value (copy). Asserts on error/cancellation. BLOCKS the calling
/// thread (see try_async_blocking_get). For fallible handling use try_async_blocking_get; for zero-copy
/// access use root->try_value() after driving.
template <class T>
[[nodiscard]] T async_blocking_get(shared_async<T> const& root)
{
    auto r = cc::try_async_blocking_get(root);
    CC_ASSERT(r.has_value(), "async completed with an error or was cancelled");
    return cc::move(r).value();
}
} // namespace cc
