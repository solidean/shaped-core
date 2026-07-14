#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async_node.hh>

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
//   shared_async<T> = cc::shared_ptr<async<T>, ...>  — the normal, composable, many-dependent handle (an 8 B
//     intrusive-refcount handle over one slab node). async<T> itself is non-copyable and immovable; you copy
//     the handle, never the node.
//
// Design notes worth stating up front:
//   * pending-dependency lists hold only NOT-ready deps, purely for scheduling/wakeup.
//   * compute-frame captures are what retain observed shared_async dependencies (not the pending list).
//   * subscriptions/continuations are late-installed wakeup hints, added only when a frame must park.

namespace cc
{
// shared_async<T> = cc::shared_ptr<async<T>, impl::async_node_traits> — defined in clean-core/fwd.hh so light
// forward headers (e.g. shaped-graphics fwd.hh) can name the handle without pulling in the whole async surface.

// ============================================================================
// compute-frame result plumbing
// ============================================================================

namespace impl
{
// local declval (clean-core does not bless <utility>): produces a value of type U in unevaluated context
template <class U>
U async_declval() noexcept;
} // namespace impl

// ============================================================================
// typed node: the value + compute-frame layer under async<T>
// ============================================================================

namespace impl
{
/// Declares the node's payload storage (the offset-16 slot the base manages, see async_node_base::payload()):
/// raw bytes holding EITHER the unresolved scratch (frame + deps + conts, ~32 B) OR the resolved value ⊍ error.
/// Sized to the larger of the scratch and sizeof(T), so the value grows the node naturally for a large T (the
/// node stays one cache line for sizeof(T) up to ~48 B; a larger T spills onto more lines). Cacheline-aligned so
/// unrelated nodes never share a line; the value is 16-aligned. This layer adds only the typed read/teardown of
/// the value — the base owns everything else (including the compute frame, in the payload's unresolved arm).
template <class T>
struct alignas(64) async_typed_node : cc::async_node_base
{
    static_assert(alignof(T) <= 16, "async<T>: T is over-aligned (> 16) — box it");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "async<T>: T must be nothrow-move-constructible (it is moved under the node lock at completion)");

    /// Destroy the resolved value in the payload (called through the ops table at teardown when ready_value).
    void clear_value() { reinterpret_cast<T*>(this->value_storage())->~T(); }

    /// Pointer to the produced value; null unless ready with a value. Stable while the node is alive.
    [[nodiscard]] T const* value_ptr() const
    {
        return this->has_value() ? reinterpret_cast<T const*>(this->payload()) : nullptr;
    }
    [[nodiscard]] T* value_ptr() { return this->has_value() ? reinterpret_cast<T*>(this->payload()) : nullptr; }

private:
    static constexpr cc::isize payload_bytes
        = sizeof(T) > sizeof(impl::async_unresolved) ? cc::isize(sizeof(T)) : cc::isize(sizeof(impl::async_unresolved));
    alignas(16) cc::byte _payload[payload_bytes]; // the offset-16 slot; base reaches it via payload()
};

/// Type-erased ops for async<T> (the hand-rolled vtable, see cc::async_type_ops): destroy the resolved value +
/// the concrete size class, reached from the untemplated base. async<T> adds no data members over
/// async_typed_node<T>, so their size classes match (class index taken from async_typed_node<T>, complete here;
/// async<T> is not yet). The async<T> ctor static_asserts the sizes stay equal.
template <class T>
struct async_typed_node_ops
{
    static void teardown_value(cc::async_node_base* n) { static_cast<async_typed_node<T>*>(n)->clear_value(); }
};
template <class T>
inline constexpr cc::async_type_ops async_type_ops_for = {
    &async_typed_node_ops<T>::teardown_value,
    cc::node_class_index_for<async_typed_node<T>>(),
};
} // namespace impl

// ============================================================================
// async<T> — the normal shared handle
// ============================================================================

/// The normal composable async handle. Always used through shared_async<T> (an intrusive cc::shared_ptr); the
/// node is non-copyable and immovable. Create with cc::make_async_lazy / cc::make_async_scheduled (the variadic
/// dependency form handles single- and multi-dependency transforms), drive with cc::async_blocking_get.
template <class T>
struct async : impl::async_typed_node<T>
{
    /// Install this concrete type's ops (its static async_type_ops) for the intrusive free + typed-value
    /// teardown paths. Nodes are only ever created via make_async_* / cc::make_shared<async<T>>, which allocate
    /// exactly node_class_index_for<async<T>>() — matching the ops' class_index (async<T> adds no data members).
    async()
    {
        static_assert(sizeof(async) == sizeof(impl::async_typed_node<T>),
                      "async<T> must add no data members over async_typed_node<T> (ops class_index relies on it)");
        this->set_ops(&impl::async_type_ops_for<T>);
        this->init_payload(); // birth the unresolved arm (empty frame/deps/conts) into the offset-16 slot
    }

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
    void push_value(T v) { this->finish_value(cc::move(v)); } // builds the value into the payload, wakes dependents

    /// Complete externally with an error; wakes any parked dependents. Call at most once.
    void push_error(async_error e) { this->complete_with_error(cc::move(e)); }
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
    async_error* out_error = nullptr; // the poll loop's per-step error slot; resolve_to_error writes here

    // resolving the result — each returns the matching async_step_status, so a frame can `return ctx.xxx(...)`
public:
    /// Resolve this frame with a value. Builds the value straight into the node's payload and completes the node
    /// in place (the frame is a separate member the value never overlaps, so this is safe while the frame runs).
    /// Reports produced_value; call at most once. V must match the node's result type. AFTER resolving, the frame
    /// is spent — it must not touch the node again.
    template <class V>
    [[nodiscard]] async_step_status resolve_to_value(V&& v) const
    {
        current->finish_value(cc::forward<V>(v));
        return async_step_status::produced_value;
    }

    /// Resolve this frame on the failure channel. The error is handed back to the poll loop (via out_error),
    /// which installs it into the node's payload at completion — it is not stored on the node now, whose
    /// payload still holds the live continuation head. Reports produced_error.
    [[nodiscard]] async_step_status resolve_to_error(async_error e) const
    {
        *out_error = cc::move(e);
        return async_step_status::produced_error;
    }

    // convenience aliases for readable frames: success/error resolve; wait/yield report a status
public:
    template <class V>
    [[nodiscard]] async_step_status success(V&& v) const
    {
        return resolve_to_value(cc::forward<V>(v));
    }
    [[nodiscard]] async_step_status error(async_error e) const { return resolve_to_error(cc::move(e)); }
    [[nodiscard]] async_step_status error(cc::any_error e) const
    {
        return resolve_to_error(async_error::make_error(cc::move(e)));
    }
    [[nodiscard]] async_step_status wait_for_dependencies() const { return async_step_status::waiting; }
    [[nodiscard]] async_step_status yield() const { return async_step_status::yield; }

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

// invoke the user function and resolve the node with its result. A raw frame that itself returns a status
// (it resolves via ctx and manages its own dependencies) is passed through unchanged; any other return value
// is a plain value that we resolve into the node here.
template <class F, class... Args>
async_step_status async_invoke_and_resolve(async_context& ctx, F& f, Args&&... args)
{
    using r_t = std::remove_cvref_t<decltype(async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...))>;
    if constexpr (std::is_same_v<r_t, async_step_status>)
        return async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...);
    else
        return ctx.resolve_to_value(async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...));
}

// deduced value type of a frame f applied to the unwrapped dependency arguments (with or without ctx). A raw
// frame that returns async_step_status carries no value type — those must give the result type explicitly.
template <class F, class... Deps>
using async_deduced_frame_result_t
    = std::remove_cvref_t<decltype(async_invoke_frame_fn(async_declval<std::decay_t<F>&>(),
                                                         async_declval<async_context&>(),
                                                         async_unwrap_arg(async_declval<std::decay_t<Deps> const&>())...))>;

// The one compute-frame wrapper behind every make_async_* form. Requires all deps, short-circuits on the first
// error, then invokes f — passing async_context& only if f wants it, and resolving f's returned value (or
// passing through the status of a raw ctx-resolving frame). Returns the step status. A raw state-machine frame
// is just the zero-dependency case where f takes async_context& and resolves + returns a status itself.
template <class F, class... Deps>
auto async_make_frame(F&& f, Deps&&... deps)
{
    return [fn = cc::forward<F>(f), ... ds = cc::forward<Deps>(deps)](async_context& ctx) mutable -> async_step_status
    {
        bool all_ready = true;
        ((all_ready = async_require_arg(ctx, ds) && all_ready), ...); // require EVERY dep (registers pending)
        if (!all_ready)
            return async_step_status::waiting;

        cc::optional<async_error> err;
        (async_collect_arg_error(err, ds), ...); // the first errored dependency short-circuits f
        if (err.has_value())
            return ctx.resolve_to_error(cc::move(err.value()));

        return async_invoke_and_resolve(ctx, fn, async_unwrap_arg(ds)...);
    };
}

// shared factory for make_async_*: build async<R> and install the wrapped frame
template <class T, class F, class... Deps>
auto async_make_node(F&& f, Deps&&... deps)
{
    using result_t = std::conditional_t<async_is_deduce<T>, async_deduced_frame_result_t<F, Deps...>, T>;
    static_assert(!std::is_void_v<result_t>, "the frame must return a value (wrap void as cc::unit)");
    static_assert(!std::is_same_v<result_t, async_step_status>,
                  "a raw async_context frame resolves via ctx and returns a status, not a value — give the "
                  "result type explicitly, e.g. make_async_lazy<int>(...)");

    auto node = cc::make_shared<async<result_t>, async_node_traits>();
    node->set_frame(async_make_frame(cc::forward<F>(f), cc::forward<Deps>(deps)...));
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
///   * T defaults to the deduced return type of f; a raw frame that resolves via async_context (and returns a
///     status) carries no value type, so it must give T explicitly.
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
    auto node = cc::make_shared<async<T>, impl::async_node_traits>();
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
