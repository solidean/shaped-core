#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async_node.hh>

#include <type_traits> // std::decay_t

// cc::async<T, E = async_error> — a low-overhead value/dataflow async for compute-heavy dependency graphs.
//
// The mental model is values and dataflow transformations, not futures/promises or callback chains. An
// async<T, E> is an eventual result<T, E> produced by a compute frame: a callable/state-machine polled through
// an async_context<T, E>. Polling never blocks a thread; a frame that cannot yet progress parks on its not-ready
// dependencies and is woken when they complete. E is the failure channel; the default async_error wraps a
// move-only cc::any_error, but any type works (e.g. an enum or a small struct) — see into_result / try_error.
//
//   auto a = cc::make_async_scheduled<int>([](cc::async_context<int>&) { return 40; });
//   auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);   // b depends on a; f gets a plain int
//   int v = cc::async_blocking_get_singlethreaded(b);   // drives the graph on this thread -> 42
//
// Handles:
//   shared_async<T, E> = cc::shared_ptr<async<T, E>, ...>  — the normal, composable, many-dependent handle (an
//     8 B intrusive-refcount handle over one slab node). async<T, E> itself is non-copyable and immovable; you
//     copy the handle, never the node.
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
/// raw bytes holding EITHER the unresolved scratch (frame + deps + conts, ~32 B) OR the resolved value ⊍ error
/// (value and error share offset 0, discriminated by state). Sized to the larger of the scratch, sizeof(T), and
/// sizeof(E), so the value/error grows the node naturally for a large T/E (the node stays one cache line while
/// both fit ~48 B; larger spills onto more lines). Cacheline-aligned so unrelated nodes never share a line; the
/// value/error is 16-aligned. This layer adds the typed read/teardown of the value AND the error — the base owns
/// everything else (including the compute frame, in the payload's unresolved arm).
template <class T, class E>
struct alignas(64) async_typed_node : cc::async_node_base
{
    static_assert(alignof(T) <= 16, "async<T, E>: T is over-aligned (> 16) — box it");
    static_assert(alignof(E) <= 16, "async<T, E>: E is over-aligned (> 16) — box it");
    // NOTE: no blanket nothrow-move requirement on T — an emplace-only immovable T is supported. The nothrow-move
    // constraint is applied per-path, only where a value is actually moved (finish_value / push_value /
    // make_async_from_value / into_result). Payload teardown is not a member here — it lives in the single-type
    // helper impl::async_typed_teardown so the ops descriptor collapses across types (see async_type_ops_for).

    /// Pointer to the produced value; null unless ready with a value. Stable while the node is alive.
    [[nodiscard]] T const* value_ptr() const
    {
        return this->has_value() ? reinterpret_cast<T const*>(this->payload()) : nullptr;
    }
    [[nodiscard]] T* value_ptr() { return this->has_value() ? reinterpret_cast<T*>(this->payload()) : nullptr; }

    /// Pointer to the produced error; null unless ready with an error. Stable while the node is alive.
    [[nodiscard]] E const* error_ptr() const
    {
        return this->has_error() ? reinterpret_cast<E const*>(this->payload()) : nullptr;
    }
    [[nodiscard]] E* error_ptr() { return this->has_error() ? reinterpret_cast<E*>(this->payload()) : nullptr; }

private:
    static constexpr cc::isize max_of(cc::isize a, cc::isize b) { return a > b ? a : b; }
    static constexpr cc::isize payload_bytes
        = max_of(max_of(cc::isize(sizeof(T)), cc::isize(sizeof(E))), cc::isize(sizeof(impl::async_unresolved)));
    alignas(16) cc::byte _payload[payload_bytes]; // the offset-16 slot; base reaches it via payload()
};

/// Type-erased teardown of the resolved payload: destroy a single U (the value or the error) at payload offset 0.
/// Keyed on ONE type, not the (T, E) pair — the value and error share offset 0, and this needs neither the other
/// arm's type nor the node size — so it is shared by every async whose value, or whose error, is U. (Friend of
/// async_node_base, declared in async_node.hh, to reach the protected payload.)
template <class U>
void async_typed_teardown(cc::async_node_base* n)
{
    reinterpret_cast<U*>(n->value_storage())->~U();
}

using async_teardown_fn = void (*)(cc::async_node_base*);

/// Teardown pointer for a payload of type U: null for a trivially-destructible U (no function is emitted, and the
/// null slot is what lets descriptors collapse), else the single-type teardown. `if constexpr` so a trivial U
/// never instantiates async_typed_teardown<U>.
template <class U>
consteval async_teardown_fn async_teardown_ptr()
{
    if constexpr (std::is_trivially_destructible_v<U>)
        return nullptr;
    else
        return &async_typed_teardown<U>;
}

/// Run / destroy an inline compute frame of type G, through the type-erased ops table. G is whatever set_frame
/// installed: the frame closure itself, or a boxed cc::unique_function when it was too big to store inline.
template <class G>
async_step_status async_frame_invoke(void* frame, cc::async_context_base& ctx)
{
    return (*static_cast<G*>(frame))(ctx);
}
template <class G>
void async_frame_destroy(void* frame)
{
    static_cast<G*>(frame)->~G();
}

using async_frame_invoke_fn = async_step_status (*)(void*, cc::async_context_base&);
using async_frame_destroy_fn = void (*)(void*);

/// The ops descriptor (hand-rolled vtable, see cc::async_type_ops) keyed on what actually distinguishes it: the
/// node size class, the value/error teardowns, and the frame ops. Two async types agreeing on all of those
/// share ONE static instance — e.g. async<int, async_error> and async<float, async_error> get the SAME
/// async_type_ops pointer (both: null value teardown, async_error error teardown, same 64 B class). The frame
/// ops are per-frame-type, so a framed descriptor no longer collapses across closures — fine, since each
/// closure already emits its own code; the frameless descriptors below still collapse exactly as before.
/// class_index comes from async_typed_node<T, E>, which is complete here (async<T, E> is not).
template <cc::node_class_index Cls, async_teardown_fn TV, async_teardown_fn TE, async_frame_invoke_fn FI, async_frame_destroy_fn FD>
inline constexpr cc::async_type_ops async_type_ops_v = {TV, TE, FI, FD, Cls};

/// The descriptor for a concrete async<T, E> with NO compute frame — a manual/push node, or a born-ready
/// factory. A reference into the shared, collapsed instance above.
template <class T, class E>
inline constexpr cc::async_type_ops const& async_type_ops_for
    = async_type_ops_v<cc::node_class_index_for<async_typed_node<T, E>>(), async_teardown_ptr<T>(), async_teardown_ptr<E>(), nullptr, nullptr>;

/// The descriptor for an async<T, E> running an inline frame of type G. Installed by set_frame, which is what
/// determines G — the node births frameless and is re-pointed here before it is shared.
template <class T, class E, class G>
inline constexpr cc::async_type_ops const& async_type_ops_for_frame
    = async_type_ops_v<cc::node_class_index_for<async_typed_node<T, E>>(),
                       async_teardown_ptr<T>(),
                       async_teardown_ptr<E>(),
                       &async_frame_invoke<G>,
                       &async_frame_destroy<G>>;

// error-propagation hook: produce a fresh, independent copy of a dependency's error for a dependent node.
// The default copies (a custom E is assumed copyable where this is used); the async_error overload
// re-materializes from the message because cc::any_error is move-only and a shared node's error must not be
// moved out (the context chain is lost — a richer error-sharing scheme is a follow-up). Cancellation is
// preserved. A non-template overload is chosen over the template for async_error.
template <class E>
[[nodiscard]] E async_error_propagate(E const& e)
{
    return e; // requires E copyable
}
[[nodiscard]] async_error async_error_propagate(async_error const& e);
} // namespace impl

// ============================================================================
// async<T> — the normal shared handle
// ============================================================================

namespace impl
{
/// Tag selecting async's manual/promise constructor (born external_pending in a single store). Passed by
/// make_async_manual through make_shared; not part of the public surface.
struct async_manual_tag
{
};
} // namespace impl

/// The normal composable async handle. Always used through shared_async<T, E> (an intrusive cc::shared_ptr); the
/// node is non-copyable and immovable. E is the failure-channel type, defaulting to async_error. Create with
/// cc::make_async_lazy / cc::make_async_scheduled (the variadic dependency form handles single- and
/// multi-dependency transforms) or the make_async_from_* factories, drive with cc::async_blocking_get_singlethreaded.
template <class T, class E>
struct async : impl::async_typed_node<T, E>
{
    /// Install this concrete type's ops (its static async_type_ops) for the intrusive free + typed value/error
    /// teardown paths. Nodes are only ever created via make_async_* / cc::make_shared<async<T, E>>, which
    /// allocate exactly node_class_index_for<async<T, E>>() — matching the ops' class_index (async adds no data).
    async()
    {
        check_layout();
        this->set_ops(&impl::async_type_ops_for<T, E>);
        this->init_payload(); // birth the unresolved arm (empty frame/deps/conts) into the offset-16 slot
    }

    /// The manual/promise node: born external_pending, awaiting push_value / push_error. Sets ops and the
    /// external_pending state in ONE store (the node isn't shared yet); a separate post-construction state
    /// store would not fold across the atomic control word. See make_async_manual.
    explicit async(impl::async_manual_tag)
    {
        check_layout();
        this->init_control_word(&impl::async_type_ops_for<T, E>, async_node_state::external_pending);
        this->init_payload();
    }

private:
    static void check_layout()
    {
        static_assert(sizeof(async) == sizeof(impl::async_typed_node<T, E>),
                      "async<T, E> must add no data members over async_typed_node<T, E> (ops class_index needs it)");
    }

    // compute frame
public:
    /// Install the compute frame — `async_step_status(async_context_base&)`; it resolves its outcome via the
    /// context (a typed async_context<T, E> the frame closure wraps around the base for resolve/emplace).
    /// `f` is moved in once, here; it is never moved again (see async_node_base's frame section).
    template <class F>
    void set_frame(F&& f)
    {
        set_frame_emplace<std::decay_t<F>>(cc::forward<F>(f));
    }

    /// Install the compute frame by building it in place from `args` — F never has to be movable at all.
    ///
    /// Call before the node is shared (make_async_* does): installing the frame is what determines F, and thus
    /// which ops instance the node points at, which is written with a plain relaxed store.
    template <class F, class... Args>
    void set_frame_emplace(Args&&... args)
    {
        if constexpr (async::template frame_fits_inline<F>)
            this->template install_frame<F>(&impl::async_type_ops_for_frame<T, E, F>, cc::forward<Args>(args)...);
        else
        {
            // too big for the inline slot: box it. A cc::unique_function is one pointer, so IT fits inline, and
            // create_from emplaces the closure into the box — an immovable F still works.
            using boxed_t = typename async::frame_type;
            this->template install_frame<boxed_t>(
                &impl::async_type_ops_for_frame<T, E, boxed_t>,
                boxed_t::template create_from<F>(cc::default_node_allocator(), cc::forward<Args>(args)...));
        }
    }

    // zero-copy access
public:
    /// Pointer to the stored value, or null unless ready with a value. Non-owning — valid while the node is
    /// alive (you keep it alive through the shared_async handle, not through this pointer).
    [[nodiscard]] T const* try_value() const { return this->value_ptr(); }

    /// Pointer to the failure-channel value (typed E), or null unless ready with an error. Non-owning — valid
    /// while the node is alive.
    [[nodiscard]] E const* try_error() const { return this->error_ptr(); }

    /// A fresh, independent copy of this node's error for propagation to a dependent (see async_error_propagate):
    /// a move-only async_error is re-materialized from its message; a copyable custom E is copied. Requires
    /// has_error().
    [[nodiscard]] E propagate_error() const
    {
        CC_ASSERT(this->has_error(), "no error to propagate");
        // unqualified template arg: the async_error overload wins for E = async_error (move-only, can't copy);
        // the copy template is selected for a copyable custom E.
        return impl::async_error_propagate(*this->error_ptr());
    }

    // manual / promise-style completion (for externally produced values)
public:
    /// Complete externally with a value; wakes any parked dependents. Call at most once.
    void push_value(T v) { this->finish_value(cc::move(v)); } // builds the value into the payload, wakes dependents

    /// Complete externally with a value built in place from `args` (never moved) — the immovable-T path.
    template <class... Args>
    void push_value_emplace(Args&&... args)
    {
        this->template finish_value_emplace<T>(cc::forward<Args>(args)...);
    }

    /// Complete externally with an error; wakes any parked dependents. Call at most once.
    void push_error(E e) { this->finish_error(cc::move(e)); }

    /// Complete externally with an error built in place from `args`.
    template <class... Args>
    void push_error_emplace(Args&&... args)
    {
        this->template finish_error_emplace<E>(cc::forward<Args>(args)...);
    }
};

// ============================================================================
// async_context — handed to every compute step
// ============================================================================

/// The T/E-agnostic half of the compute-step context: what the poll loop and the type-erased stored frame name
/// (frame_type is async_step_status(async_context_base&)). It reads dependencies and reports wait/yield; the
/// typed async_context<T, E> wraps it to add the value/error resolves. Not owned by the frame; valid only for the
/// duration of a single step.
struct async_context_base
{
    async_node_base* current = nullptr;
    async_scheduler* scheduler = nullptr;

    // report-a-status helpers (no T/E needed)
public:
    [[nodiscard]] async_step_status wait_for_dependencies() const { return async_step_status::waiting; }
    [[nodiscard]] async_step_status yield() const { return async_step_status::yield; }

    // dependencies
public:
    /// Require an existing async as a dependency. Returns true if it is already ready (read its value now);
    /// otherwise records it as a pending dependency and returns false — the frame should then return
    /// wait_for_dependencies().
    ///
    /// Neither subscribes nor schedules: the poll loop owns both. It drives one dependency inline on this
    /// stack, publishes the rest only if some other thread could actually steal them, and schedules whatever
    /// is left before it parks. Scheduling here instead would enqueue a node we are about to run ourselves.
    template <class T, class E>
    bool require(shared_async<T, E> const& dep) const
    {
        CC_ASSERT(dep != nullptr, "cannot require a null async");
        CC_ASSERT(!current->is_ready(), "this async already resolved — a spent frame must not touch its context");
        if (dep->is_ready())
            return true;
        current->add_pending_dependency(dep.get());
        return false;
    }
};

/// The typed compute-step context the user's frame receives: `[](cc::async_context<T, E>& ctx) -> ...`. Carries
/// no extra state — it is a thin wrapper the frame closure builds around the async_context_base for the step,
/// adding the value/error resolves (which need to know T/E). Inherits require / wait_for_dependencies / yield.
template <class T, class E>
struct async_context : async_context_base
{
    /// Wrap the step's base context (copies the two pointers).
    async_context(async_context_base const& base) : async_context_base(base) {}

    // resolving the result — each returns the matching async_step_status, so a frame can `return ctx.xxx(...)`.
    // Resolving completes the node IN PLACE, over the frame's own slot: it destroys the frame — the live,
    // executing closure — and builds the value/error there. So resolve is terminal in the `delete this;` sense:
    // the frame must not touch its captures, or the context, afterwards. `return ctx.success(v);` is the shape.
public:
    /// Resolve with a value (moved into the payload). Anything convertible to T works — by value, so the
    /// conversion to T happens at the call site: what reaches the node is a stack temporary of the payload's
    /// exact type, never a reference into (say) a dependency's payload that the frame's captures pin alive.
    [[nodiscard]] async_step_status resolve_to_value(T v) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        current->finish_value(cc::move(v)); // T exactly, so finish_value's decay deduces the payload type
        return async_step_status::produced_value;
    }

    /// Resolve with a value built in place from `args` (never moved) — the immovable-T path.
    ///
    /// `args` are forwarded by reference into the payload slot, and resolving destroys the frame first, so they
    /// must NOT reference the frame's own captures (nor anything only those captures keep alive, e.g. a
    /// dependency's value). Pass owned or stack-local arguments; resolve_to_value has no such caveat.
    template <class... Args>
    [[nodiscard]] async_step_status resolve_to_value_emplace(Args&&... args) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        current->template finish_value_emplace<T>(cc::forward<Args>(args)...);
        return async_step_status::produced_value;
    }

    /// Resolve on the failure channel with E (moved into the payload).
    [[nodiscard]] async_step_status resolve_to_error(E e) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        current->finish_error(cc::move(e));
        return async_step_status::produced_error;
    }

    /// Resolve on the failure channel with an error built in place from `args`. Same aliasing caveat as
    /// resolve_to_value_emplace: `args` must not reference the frame's captures.
    template <class... Args>
    [[nodiscard]] async_step_status resolve_to_error_emplace(Args&&... args) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        current->template finish_error_emplace<E>(cc::forward<Args>(args)...);
        return async_step_status::produced_error;
    }

    // convenience aliases for readable frames
public:
    [[nodiscard]] async_step_status success(T v) const { return resolve_to_value(cc::move(v)); }
    [[nodiscard]] async_step_status error(E e) const { return resolve_to_error(cc::move(e)); }

    /// Only for the default failure channel: wrap a cc::any_error as an async_error. Enabled when E is async_error.
    [[nodiscard]] async_step_status error(cc::any_error e) const
        requires(std::is_same_v<E, async_error>)
    {
        return resolve_to_error(async_error::make_error(cc::move(e)));
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

// A dependency argument is a shared_async<U, Ue>: awaited before f runs, then unwrapped to the stored U. Plain
// pass-through values are not supported yet — capture them in the closure.

template <class U, class Ue>
bool async_require_arg(async_context_base& ctx, shared_async<U, Ue> const& dep)
{
    return ctx.require(dep);
}

// Collect the first errored dep's error, propagated into THIS node's error type E (see async_error_propagate).
// The high-level sugar assumes a single failure type across the graph: a dep's propagated error (its own Ue)
// must be constructible into E. For the default async_error everywhere this is the identity.
template <class E, class U, class Ue>
void async_collect_arg_error(cc::optional<E>& out, shared_async<U, Ue> const& dep)
{
    if (!out.has_value() && dep->has_error())
        out.emplace_value(dep->propagate_error());
}

// returns a reference into the dependency node's stored value (stable while the node is alive)
template <class U, class Ue>
U const& async_unwrap_arg(shared_async<U, Ue> const& dep)
{
    return *dep->value_ptr();
}

// invoke the user function, with the leading typed async_context<T, E>& if it takes one, else without it
template <class T, class E, class F, class... Args>
decltype(auto) async_invoke_frame_fn(F& f, async_context<T, E>& ctx, Args&&... args)
{
    if constexpr (std::is_invocable_v<F&, async_context<T, E>&, Args...>)
        return f(ctx, cc::forward<Args>(args)...);
    else
        return f(cc::forward<Args>(args)...);
}

// invoke the user function and resolve the node with its result. A raw frame that itself returns a status
// (it resolves via ctx and manages its own dependencies) is passed through unchanged; any other return value
// is a plain value that we resolve into the node here.
template <class T, class E, class F, class... Args>
async_step_status async_invoke_and_resolve(async_context<T, E>& ctx, F& f, Args&&... args)
{
    using r_t = std::remove_cvref_t<decltype(async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...))>;
    if constexpr (std::is_same_v<r_t, async_step_status>)
        return async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...);
    else
        return ctx.resolve_to_value(async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...));
}

// deduced value type of a value-returning frame f applied to the unwrapped dependency arguments (WITHOUT ctx).
// Deduction is context-free: the typed context's own T is what we are deducing, so a value frame that also takes
// a ctx cannot be deduced — those (like every raw async_step_status frame) must give the result type explicitly.
template <class F, class... Deps>
using async_deduced_frame_result_t = std::remove_cvref_t<decltype(async_declval<std::decay_t<F>&>()(
    async_unwrap_arg(async_declval<std::decay_t<Deps> const&>())...))>;

// The node's value type: T verbatim, or (only when T is the deduce sentinel) the deduced frame result. A partial
// specialization keeps the deduction LAZY — async_deduced_frame_result_t (which invokes f without a ctx) must not
// be instantiated when T is given explicitly, e.g. for a raw ctx-resolving frame that cannot be called ctx-free.
template <class T, class F, class... Deps>
struct async_result_type
{
    using type = T;
};
template <class F, class... Deps>
struct async_result_type<async_deduce_result, F, Deps...>
{
    using type = async_deduced_frame_result_t<F, Deps...>;
};

// The one compute-frame wrapper behind every make_async_* form. Its stored signature is the type-erased
// async_context_base&; it builds the typed async_context<Result, E> for the step, requires all deps,
// short-circuits on the first error, then invokes f — passing the typed context only if f wants it, and resolving
// f's returned value (or passing through the status of a raw ctx-resolving frame). A raw state-machine frame is
// just the zero-dependency case where f takes async_context<Result, E>& and resolves + returns a status itself.
template <class Result, class E, class F, class... Deps>
auto async_make_frame(F&& f, Deps&&... deps)
{
    return [fn = cc::forward<F>(f), ... ds = cc::forward<Deps>(deps)](async_context_base& base) mutable -> async_step_status
    {
        async_context<Result, E> ctx(base);

        bool all_ready = true;
        ((all_ready = async_require_arg(ctx, ds) && all_ready), ...); // require EVERY dep (registers pending)
        if (!all_ready)
            return async_step_status::waiting;

        cc::optional<E> err;
        (async_collect_arg_error(err, ds), ...); // the first errored dependency short-circuits f
        if (err.has_value())
            return ctx.resolve_to_error(cc::move(err.value()));

        return async_invoke_and_resolve(ctx, fn, async_unwrap_arg(ds)...);
    };
}

// The emplace twin of async_make_frame's lambda. That lambda captures `fn` by move, which an immovable F
// cannot do; this holds F as a member built in place from the caller's args. No dependency arguments — the
// variadic dep form has to move its shared_async handles in anyway, so it stays on the lambda.
template <class Result, class E, class F>
struct async_frame_holder
{
    F fn;

    template <class... Args>
    explicit async_frame_holder(Args&&... args) : fn(cc::forward<Args>(args)...)
    {
    }

    async_step_status operator()(async_context_base& base)
    {
        async_context<Result, E> ctx(base);
        return async_invoke_and_resolve(ctx, fn);
    }
};

// shared factory for make_async_*: build async<R, E> and install the wrapped frame
template <class T, class E, class F, class... Deps>
auto async_make_node(F&& f, Deps&&... deps)
{
    using result_t = async_result_type<T, F, Deps...>::type;
    static_assert(!std::is_void_v<result_t>, "the frame must return a value (wrap void as cc::unit)");
    static_assert(!std::is_same_v<result_t, async_step_status>,
                  "a raw async_context frame resolves via ctx and returns a status, not a value — give the "
                  "result type explicitly, e.g. make_async_lazy<int>(...)");

    auto node = cc::make_shared<async<result_t, E>, async_node_traits>();
    node->set_frame(async_make_frame<result_t, E>(cc::forward<F>(f), cc::forward<Deps>(deps)...));
    return node;
}
} // namespace impl

// ============================================================================
// creation
// ============================================================================

/// Create a cold (lazy) async — it runs only once scheduled (required by another async, or driven by
/// async_blocking_get_singlethreaded). The compute frame is `f`, optionally followed by dependency arguments:
///
///   * each dependency is a shared_async<U>; it is awaited and unwrapped to the stored U before f runs.
///     Errors short-circuit: if any dependency failed, f is skipped and the first error propagates.
///   * f is called with the unwrapped dependency values. It may take a leading async_context<T, E>& (for a raw
///     state-machine that manages its own dependencies) or omit it entirely — both are wrapped as needed.
///   * T defaults to the deduced return type of f (deduction is context-free — a value frame that also takes a
///     ctx, like every raw async_context frame that returns a status, must give T explicitly). E is the failure
///     channel, defaulting to async_error.
///
///   auto a = cc::make_async_scheduled<int>([](cc::async_context<int>&) { return 40; }); // raw frame
///   auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);                       // depends on a; f gets int
///   auto c = cc::make_async_lazy([] { return 7; });                                     // no deps, no context
template <class T = impl::async_deduce_result, class E = async_error, class F, class... Deps>
[[nodiscard]] auto make_async_lazy(F&& f, Deps&&... deps)
{
    return impl::async_make_node<T, E>(cc::forward<F>(f), cc::forward<Deps>(deps)...);
}

/// Like make_async_lazy, but builds the frame in place from `args` rather than moving a callable in — so an
/// IMMOVABLE frame works (one with a pinned capture, a hand-written state machine that cannot move). The frame
/// is never moved after installation either, not even when it parks.
///
/// T must be given explicitly: deducing it would mean invoking F, and F does not exist yet. No dependency
/// arguments — capture what the frame needs and require() it, or use make_async_lazy.
///
///   auto n = cc::make_async_lazy_emplace<int, cc::async_error, my_pinned_frame>(7);
template <class T, class E = async_error, class F, class... Args>
[[nodiscard]] shared_async<T, E> make_async_lazy_emplace(Args&&... args)
{
    auto node = cc::make_shared<async<T, E>, impl::async_node_traits>();
    node->template set_frame_emplace<impl::async_frame_holder<T, E, F>>(cc::forward<Args>(args)...);
    return node;
}

/// Like make_async_lazy, but eager: schedules the node immediately if a worker scope is active on this thread
/// (otherwise it stays cold and is scheduled when first required/driven). Same forms as make_async_lazy.
template <class T = impl::async_deduce_result, class E = async_error, class F, class... Deps>
[[nodiscard]] auto make_async_scheduled(F&& f, Deps&&... deps)
{
    auto node = impl::async_make_node<T, E>(cc::forward<F>(f), cc::forward<Deps>(deps)...);
    if (async_scheduler::current_or_null() != nullptr)
        node->schedule();
    return node;
}

/// Create an async completed externally via async<T>::push_value / push_error (a promise-style node). A
/// dependent that requires it parks until it is pushed.
template <class T, class E = async_error>
[[nodiscard]] shared_async<T, E> make_async_manual()
{
    // The manual-tag ctor births the node external_pending in one store.
    return cc::make_shared<async<T, E>, impl::async_node_traits>(impl::async_manual_tag{});
}

// ============================================================================
// creation — born already ready (no frame, no scheduling)
// ============================================================================

/// An async that is already ready with `v` — no compute frame, no scheduling. Usable immediately as a
/// dependency or read via try_value(). Moves `v` in (T must be nothrow-move-constructible; use the _emplace
/// form for an immovable T).
template <class T, class E = async_error>
[[nodiscard]] shared_async<T, E> make_async_from_value(T v)
{
    auto node = cc::make_shared<async<T, E>, impl::async_node_traits>();
    node->push_value(cc::move(v)); // cold node, empty unresolved arm -> drives straight to ready_value
    return node;
}

/// Like make_async_from_value, but builds the value in place from `args` (never moved) — the immovable-T path.
/// T is explicit (args do not determine it): make_async_from_value_emplace<T>(args...).
template <class T, class E = async_error, class... Args>
[[nodiscard]] shared_async<T, E> make_async_from_value_emplace(Args&&... args)
{
    auto node = cc::make_shared<async<T, E>, impl::async_node_traits>();
    node->push_value_emplace(cc::forward<Args>(args)...);
    return node;
}

/// An async that is already ready on the failure channel with `e`. T is explicit (only E is determined by the
/// argument): make_async_from_error<T>(e).
template <class T, class E = async_error>
[[nodiscard]] shared_async<T, E> make_async_from_error(E e)
{
    auto node = cc::make_shared<async<T, E>, impl::async_node_traits>();
    node->push_error(cc::move(e)); // cold node, empty unresolved arm -> drives straight to ready_error
    return node;
}

/// Like make_async_from_error, but builds the error in place from `args`. T and E are explicit:
/// make_async_from_error_emplace<T, E>(args...).
template <class T, class E = async_error, class... Args>
[[nodiscard]] shared_async<T, E> make_async_from_error_emplace(Args&&... args)
{
    auto node = cc::make_shared<async<T, E>, impl::async_node_traits>();
    node->push_error_emplace(cc::forward<Args>(args)...);
    return node;
}

// ============================================================================
// consuming — move the outcome out
// ============================================================================

/// Consume a READY async handle and MOVE its outcome out into a cc::result<T, E>: ready_value -> the moved value,
/// ready_error -> cc::error(moved error). Requires root to be ready. Unlike a getter, this MOVES the payload out
/// of the shared node — any OTHER live handle's later try_value()/try_error() then reads a moved-from value, so
/// use it when you are done with the async. T must be move-constructible (a truly immovable T cannot be
/// into_result'd — that is a compile error by design). Takes the handle by value, consuming the caller's handle.
template <class T, class E = async_error>
[[nodiscard]] cc::result<T, E> into_result(shared_async<T, E> root)
{
    static_assert(std::is_move_constructible_v<T>, "into_result moves the value out of the node — T must be "
                                                   "move-constructible");
    CC_ASSERT(root != nullptr, "cannot consume a null async");
    CC_ASSERT(root->is_ready(), "into_result requires a ready async (drive it first)");

    if (root->has_error())
        return cc::error(cc::move(*root->error_ptr()));
    return cc::move(*root->value_ptr());
}

// ============================================================================
// driving — a scheduler makes progress; blocking is its convenience
// ============================================================================

// You never block on an async — a scheduler drives it, and these block the CALLING thread while that happens.
// Mirrors async_thread_pool::blocking_get / try_blocking_get, with one deliberate difference: this scheduler
// never blocks, so "pumped out, still not ready" is a real outcome for it and try_ returns an optional. The
// pool waits on a completion latch instead, so it has no such outcome and returns the result directly.

template <class T, class E>
cc::optional<cc::result<T, E>> singlethreaded_scheduler::try_blocking_get(shared_async<T, E> const& root)
{
    CC_ASSERT(root != nullptr, "cannot drive a null async");

    async_worker_scope scope(*this); // nests harmlessly if this scheduler is already bound here

    root->schedule();
    run_until([&] { return root->is_ready(); });

    // Pump anything still queued out of the `scheduled` state before we stop driving. run_until stops the moment
    // `root` is ready, which can leave a foreign node that MIGRATED into our queue mid-drive (a dependency
    // completing on this thread woke a dependent that another scheduler had parked; route_after_schedule enqueued
    // it here). Left `scheduled` in a queue we no longer pump, that node is stranded: schedule()/schedule_on() are
    // idempotent on `scheduled`, so no other scheduler could reclaim it and a blocking_get on it would hang.
    // Draining now (with our worker scope still bound, so children route correctly) settles each such node into a
    // self-consistent state — completed, or re-parked as `blocked` and re-woken later onto whichever scheduler
    // finishes its dependency. No-op in the common case: publish-all-but-one leaves the queue empty once `root` is
    // ready. This does run migrated-in work single-threaded here rather than on its origin scheduler — accepted,
    // the same single-threading a shared subtree already incurs when this scheduler drives it (see
    // libs/base/clean-core/docs/systems/async.md "Multi-scheduler correctness").
    drain();

    // A drained queue does not mean the graph completed — only that WE could not advance `root`: it may be parked
    // on a manual node awaiting an external push, or have migrated onto another scheduler that is still driving it.
    // Neither is ours to assert on; report it and let the caller push, retry, or wait.
    if (!root->is_ready())
        return cc::nullopt;

    if (root->has_error())
        return cc::result<T, E>(cc::error(root->propagate_error()));
    return cc::result<T, E>(*root->value_ptr()); // copy out
}

template <class T, class E>
T singlethreaded_scheduler::blocking_get(shared_async<T, E> const& root)
{
    auto r = try_blocking_get(root);
    CC_ASSERT(r.has_value(), "async graph could not complete on this scheduler (parked on an external push that "
                             "never came, or being driven by another scheduler — use try_blocking_get)");
    CC_ASSERT(r.value().has_value(), "async completed with an error or was cancelled");
    return cc::move(r).value().value();
}

/// Drive `root` to completion on a throwaway singlethreaded_scheduler and return its outcome, or nullopt if it
/// could not complete here (e.g. parked on a manual node that is never pushed — a throwaway scheduler cannot
/// be pumped around the external push, so keep a singlethreaded_scheduler for that). BLOCKS the calling thread,
/// and the whole graph runs HERE — no dependency executes concurrently, however many cores are idle. Never call
/// it from inside a frame (park on a dependency instead).
///
/// The verbose name is deliberate: this is a top-level / test convenience. Real work belongs on a scheduler
/// you own — a singlethreaded_scheduler you pump, or an async_thread_pool.
template <class T, class E = async_error>
[[nodiscard]] cc::optional<cc::result<T, E>> try_async_blocking_get_singlethreaded(shared_async<T, E> const& root)
{
    singlethreaded_scheduler scheduler;
    return scheduler.try_blocking_get(root);
}

/// try_async_blocking_get_singlethreaded, but returns the value (copy) and asserts on error/cancellation. For
/// zero-copy access use root->try_value() after driving.
template <class T, class E = async_error>
[[nodiscard]] T async_blocking_get_singlethreaded(shared_async<T, E> const& root)
{
    singlethreaded_scheduler scheduler;
    return scheduler.blocking_get(root);
}
} // namespace cc
