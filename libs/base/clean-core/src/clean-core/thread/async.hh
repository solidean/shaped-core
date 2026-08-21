#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh> // std::decay_t
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async_node.hh>

#include <type_traits>

// cc::async<T, E = async_error> — a low-overhead value/dataflow async for compute-heavy dependency graphs.
//
// The model is values and dataflow transformations, not futures/promises or callback chains, and it is
// documented in libs/base/clean-core/docs/systems/async.md — this header is the typed public surface over async_node.hh's core.
// An async<T, E> is an eventual result<T, E> produced by a compute frame: a callable or state machine polled
// through an async_context<T, E>.
// Polling never blocks a thread; a frame that cannot yet progress parks on its not-ready dependencies and is
// woken when they complete.
// E is the failure channel, defaulting to async_error over a move-only cc::any_error, but any type works.
//
//   auto a = cc::make_async_scheduled<int>([](cc::async_context<int>&) { return 40; });
//   auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);   // b depends on a; f gets a plain int
//   int v = cc::async_blocking_get_singlethreaded(b);   // drives the graph on this thread -> 42
//
// Three ownership rules bind everything below:
//   * a pending-dependency list holds only NOT-ready deps, purely for scheduling and wakeup — it owns nothing;
//   * compute-frame captures are what retain the observed shared_async dependencies;
//   * subscriptions and continuations are late-installed wakeup hints, added only when a frame must park.

namespace cc
{
// shared_async<T> = cc::shared_ptr<async<T>, impl::async_node_traits>, defined in clean-core/fwd.hh.
// That is so a light forward header (shaped-graphics' fwd.hh, say) can name the handle without pulling in the whole async surface.

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
// exceptions escaping a compute frame
// ============================================================================

// A frame that throws instead of resolving is failed on its own channel E, so the node stays a value/error machine from the outside.
// Containment is not politeness: an escaping exception would leave the node `running` forever, parking every dependent, and terminate the process outright on a pool worker.
// The catch itself is in async_node_base::invoke_frame_step, which is untyped and reaches E only through the ops-table slot minted below.

namespace custom
{
/// Opt a failure channel E into exception containment: build an E describing an exception that escaped a frame.
///
/// The primary template is deliberately EMPTY, so an E that declares no mapping is a RUNTIME diagnostic — the node rethrows — and never a compile error.
/// That is what keeps a custom E which has no way to represent an exception, an enum say, compiling exactly as before.
///
/// Specialize with a single `static E make(cc::string_view message)`.
/// The message is already extracted, so a specialization needs neither <exception> nor exception support at all.
template <class E>
struct async_error_from_exception_trait
{
};

/// The default channel's mapping: an escaped exception is an ordinary failure, never a cancellation.
template <>
struct async_error_from_exception_trait<cc::async_error>
{
    static async_error make(cc::string_view message);
};
} // namespace custom

namespace impl
{
/// Whether E can be built from an escaped exception — see cc::custom::async_error_from_exception_trait.
template <class E>
concept async_error_from_exception_capable
    = requires(cc::string_view m) { cc::custom::async_error_from_exception_trait<E>::make(m); };

/// Describe the exception CURRENTLY BEING HANDLED: a std::exception's what(), or a fixed text for anything else.
/// Callable only from inside a catch handler — it rethrows internally to classify, and a bare throw with nothing in flight terminates.
/// Out-of-line and untemplated because it is the one place needing <exception>, which keeps that out of this header.
[[nodiscard]] cc::string async_describe_current_exception();
} // namespace impl

// ============================================================================
// typed node: the value + compute-frame layer under async<T>
// ============================================================================

namespace impl
{
/// Declares the node's payload storage — the offset-16 slot the base manages, see async_node_base::payload().
/// Raw bytes holding EITHER the unresolved arm followed by the compute frame, OR the resolved value ⊍ error, which share offset 0 and are discriminated by state.
/// Sized to the larger of sizeof(T), sizeof(E) and the 48 B floor, so a large T or E grows the node naturally.
/// Cacheline-aligned so unrelated nodes never share a line; the value/error is 16-aligned.
/// This layer adds the typed read and teardown of the value AND the error, plus the inline-frame budget; the base owns everything else, including the frame's lifetime.
template <class T, class E>
struct alignas(64) async_typed_node : cc::async_node_base
{
    static_assert(alignof(T) <= 16, "async<T, E>: T is over-aligned (> 16) — box it");
    static_assert(alignof(E) <= 16, "async<T, E>: E is over-aligned (> 16) — box it");
    // No blanket nothrow-move requirement on T — an emplace-only immovable T is supported.
    // The constraint is applied per-path, only where a value is actually moved: finish_value, push_value, make_async_from_value, into_result.
    // Payload teardown is not a member here: it lives in impl::async_typed_teardown, keyed on one type, so the ops descriptor collapses (see async_type_ops_for).

    /// Pointer to the produced value; null unless ready with a value.
    /// Stable while the node is alive.
    [[nodiscard]] T const* value_ptr() const
    {
        return this->has_value() ? reinterpret_cast<T const*>(this->payload()) : nullptr;
    }

    /// Pointer to the produced error; null unless ready with an error.
    /// Stable while the node is alive.
    [[nodiscard]] E const* error_ptr() const
    {
        return this->has_error() ? reinterpret_cast<E const*>(this->payload()) : nullptr;
    }

    // The mutating twins are protected on purpose: async<T const, E> inherits this layer, and a public T*
    // here would hand a view holder write access to the payload, which is the one thing the view exists to deny.
    // async<T, E> republishes them as try_mutable_value / mutable_value / take_value / take_error.
protected:
    [[nodiscard]] T* value_ptr() { return this->has_value() ? reinterpret_cast<T*>(this->payload()) : nullptr; }
    [[nodiscard]] E* error_ptr() { return this->has_error() ? reinterpret_cast<E*>(this->payload()) : nullptr; }

private:
    static constexpr isize max_of(isize a, isize b) { return a > b ? a : b; }

    /// The floor that keeps async<int> one cache line: the 24 B arm plus a 24 B inline frame.
    /// Deliberately a constant rather than sizeof(async_unresolved).
    /// Deriving it from the arm would drop the floor to 24 the moment the arm shrank, collapsing frame_capacity to zero and boxing every frame.
    static constexpr isize payload_floor = 48;

    static constexpr isize payload_bytes = max_of(max_of(isize(sizeof(T)), isize(sizeof(E))), payload_floor);

public:
    /// Bytes the inline compute frame gets: whatever the payload has left once the unresolved arm has taken its 24.
    /// 24 B on a 64 B node — a captured fn plus two dependency handles — and it GROWS with a large T or E, since those widen the payload.
    ///
    /// So the budget is type-dependent, and the same F may box under async<int> while staying inline under async<big_thing>.
    /// Harmless, but surprising enough to be worth knowing before you go looking for why one spilled.
    static constexpr isize frame_capacity = payload_bytes - impl::async_arm_bytes;

    /// True if F is stored inline rather than boxed.
    /// Anything larger, or over-aligned, falls back to a heap-boxed cc::unique_function — itself one 8-aligned pointer, so it always fits.
    /// The alignment ceiling is 8 rather than 16 because the frame starts at an odd multiple of 8 (see impl::async_frame_align).
    template <class F>
    static constexpr bool frame_fits_inline
        = isize(sizeof(F)) <= frame_capacity && alignof(F) <= impl::async_frame_align;

private:
    alignas(16) byte _payload[payload_bytes]; // the offset-16 slot; base reaches it via payload()
};

/// Type-erased teardown of the resolved payload: destroy a single U — the value or the error — at payload offset 0.
/// Keyed on ONE type rather than the (T, E) pair, since it needs neither the other arm's type nor the node size, so every async whose value or error is U shares it.
/// A friend of async_node_base, declared in async_node.hh, to reach the protected payload.
template <class U>
void async_typed_teardown(cc::async_node_base* n)
{
    reinterpret_cast<U*>(n->value_storage())->~U();
}

using async_teardown_fn = void (*)(cc::async_node_base*);

/// Teardown pointer for a payload of type U: null for a trivially-destructible U, else the single-type teardown.
/// The null slot is what lets descriptors collapse, and `if constexpr` keeps a trivial U from ever instantiating async_typed_teardown<U>.
template <class U>
consteval async_teardown_fn async_teardown_ptr()
{
    if constexpr (std::is_trivially_destructible_v<U>)
        return nullptr;
    else
        return &async_typed_teardown<U>;
}

/// Run / destroy an inline compute frame of type G, through the type-erased ops table.
/// G is whatever set_frame installed: the frame closure itself, or a boxed cc::unique_function when it was too big to store inline.
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

/// Fail `n` on its failure channel E from the exception being handled.
/// A friend of async_node_base, declared in async_node.hh, to reach finish_error_emplace.
template <class E>
void async_frame_resolve_current_exception(cc::async_node_base* n)
{
    n->template finish_error_emplace<E>(
        cc::custom::async_error_from_exception_trait<E>::make(async_describe_current_exception()));
}

using async_frame_invoke_fn = async_step_status (*)(void*, cc::async_context_base&);
using async_frame_destroy_fn = void (*)(void*);
using async_frame_except_fn = void (*)(cc::async_node_base*);

/// The exception -> E resolver for a failure channel: null when E declares no mapping, which is the pre-existing behaviour for that E (rethrow).
/// `if constexpr` keeps a channel without a mapping from ever instantiating the thunk.
template <class E>
consteval async_frame_except_fn async_frame_except_ptr()
{
#ifdef CC_HAS_CPP_EXCEPTIONS
    if constexpr (async_error_from_exception_capable<E>)
        return &async_frame_resolve_current_exception<E>;
    else
        return nullptr;
#else
    return nullptr; // no exception can escape a frame in the first place
#endif
}

/// The ops descriptor (see cc::async_type_ops), keyed on what actually distinguishes it: the node size class, the value/error teardowns, the frame ops, and the exception resolver.
/// Two async types agreeing on all of those share ONE static instance — async<int, async_error> and async<float, async_error> get the SAME pointer.
/// The frame ops are per-frame-type, so a framed descriptor does not collapse across closures; the frameless descriptors below do.
/// class_index comes from async_typed_node<T, E>, which is complete here where async<T, E> is not.
template <cc::node_class_index Cls,
          async_teardown_fn TV,
          async_teardown_fn TE,
          async_frame_invoke_fn FI,
          async_frame_destroy_fn FD,
          async_frame_except_fn FX>
inline constexpr cc::async_type_ops async_type_ops_v = {TV, TE, FI, FD, FX, Cls};

/// The descriptor for a concrete async<T, E> with NO compute frame — a manual/push node, or a born-ready factory.
/// A reference into the shared, collapsed instance above.
/// The exception resolver is null here for the same reason the frame ops are: with no frame there is nothing that could throw.
template <class T, class E>
inline constexpr cc::async_type_ops const& async_type_ops_for
    = async_type_ops_v<cc::node_class_index_for<async_typed_node<T, E>>(),
                       async_teardown_ptr<T>(),
                       async_teardown_ptr<E>(),
                       nullptr,
                       nullptr,
                       nullptr>;

/// The descriptor for an async<T, E> running an inline frame of type G.
/// Installed by set_frame, which is what determines G — the node births frameless and is re-pointed here before it is shared.
template <class T, class E, class G>
inline constexpr cc::async_type_ops const& async_type_ops_for_frame
    = async_type_ops_v<cc::node_class_index_for<async_typed_node<T, E>>(),
                       async_teardown_ptr<T>(),
                       async_teardown_ptr<E>(),
                       &async_frame_invoke<G>,
                       &async_frame_destroy<G>,
                       async_frame_except_ptr<E>()>;

// error-propagation hook: produce a fresh, independent copy of a dependency's error for a dependent node.
// The default copies, so a custom E must be copyable where this is used.
// The async_error overload re-materializes from the message instead, because cc::any_error is move-only and PROPAGATION must never move a shared node's error out.
// That loses the context chain but preserves cancellation; a non-template overload wins over the template for async_error.
// async<T, E>::take_error is the deliberate exception — an owner may move the error out, and a node that has been taken from propagates an empty message from then on.
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
/// Whether scheduling a node from here would reach a scheduler at all.
/// async_node_base::schedule() asserts with neither a bound worker scope nor an installed default pool, so every "start it now" path tests this first.
/// A node left cold is not a lost computation — it runs when something requires it, or when a driver schedules it.
[[nodiscard]] inline bool async_can_schedule_here()
{
    return cc::async_scheduler::current_or_null() != nullptr || cc::async_scheduler::default_or_null() != nullptr;
}

/// Tag selecting async's manual/promise constructor, born external_pending in a single store.
/// Passed by make_async_manual through make_shared; not part of the public surface.
struct async_manual_tag
{
};

/// The base async<T, E> derives from: its own read-only view, async<T const, E>.
///
/// The indirection is what makes a bad T diagnosable.
/// `T const` on a reference type drops the const, so spelling the base directly would make async<T&, E> ITS OWN BASE — an
/// incomplete-type error at the base-clause, which no static_assert in the class body can intercept, because a base clause is
/// instantiated before the members.
template <class T, class E>
struct async_view_of
{
    static_assert(!std::is_reference_v<T>, "async<T, E>: T must not be a reference");
    static_assert(!std::is_void_v<T>, "async<T, E>: T must not be void — use cc::unit for a value-less async");
    static_assert(!std::is_const_v<T>,
                  "async<T const, E> is a read-only VIEW of async<T, E> — create the async<T, E> and convert");

    using type = cc::async<T const, E>;
};
} // namespace impl

} // namespace cc

/// The READ-ONLY view of an async: the same node, minus every way to change its payload.
/// Reached only by conversion — shared_async<T, E> turns into shared_async<T const, E> implicitly, and never back.
///
/// It is a BASE of async<T, E> rather than a separate node type, which is what makes the conversion free:
/// cc::shared_ptr's derived-to-base ctor does it, the storage is the same object, and there is one ops descriptor and one size class.
///
/// Read-only means the PAYLOAD.
/// Scheduling stays open — schedule(), poll() and the continuation hooks are inherited and must be, since a view holder
/// still has to be able to require and drive the node it is waiting on.
template <class T, class E>
struct cc::async<T const, E> : impl::async_typed_node<T, E>
{
    // zero-copy access
public:
    /// Pointer to the stored value, or null unless ready with a value.
    /// Non-owning, and valid only while the node is alive — you keep it alive through the shared_async handle, never through this pointer.
    [[nodiscard]] T const* try_value() const { return this->value_ptr(); }

    /// The stored value.
    /// Requires has_value().
    /// Same non-owning lifetime as try_value: the reference dies with the node, not with the expression.
    [[nodiscard]] T const& value() const
    {
        CC_ASSERT(this->has_value(), "async has no value (not ready, or ready with an error)");
        return *this->value_ptr();
    }

    /// Pointer to the failure-channel value (typed E), or null unless ready with an error.
    /// Non-owning, and valid only while the node is alive.
    [[nodiscard]] E const* try_error() const { return this->error_ptr(); }

    /// A fresh, independent copy of this node's error, for propagation to a dependent (see async_error_propagate).
    /// A move-only async_error is re-materialized from its message; a copyable custom E is copied.
    /// Requires has_error().
    [[nodiscard]] E propagate_error() const
    {
        CC_ASSERT(this->has_error(), "no error to propagate");
        // unqualified call: the async_error overload wins for E = async_error, which is move-only and cannot be copied.
        // The copy template is selected for a copyable custom E.
        return impl::async_error_propagate(*this->error_ptr());
    }

    // A view is never constructed, only converted to — async<T, E> is the only thing that may build one.
protected:
    async() = default;
};

/// The normal composable async handle, always used through shared_async<T, E> — the node itself is non-copyable and immovable.
/// E is the failure-channel type, defaulting to async_error.
/// Create with cc::make_async_lazy / cc::make_async_scheduled, whose variadic dependency form covers single- and multi-dependency transforms, or with the make_async_from_* factories.
/// Drive with cc::async_blocking_get_singlethreaded.
///
/// It derives from its own read-only view, async<T const, E>, which carries the reading half of the surface — so a handle
/// converts to shared_async<T const, E> implicitly, and that view can no longer push, take or edit the payload.
template <class T, class E>
struct cc::async : impl::async_view_of<T, E>::type
{
    /// Install this concrete type's ops for the intrusive free and typed value/error teardown paths.
    /// Nodes are only ever created via make_async_* / cc::make_shared<async<T, E>>, which allocate exactly node_class_index_for<async<T, E>>(), matching the ops' class_index.
    async()
    {
        check_layout();
        this->set_ops(&impl::async_type_ops_for<T, E>);
        this->init_payload(); // birth the unresolved arm (empty frame/deps/conts) into the offset-16 slot
    }

    /// The manual/promise node: born external_pending, awaiting push_value / push_error.
    /// Sets ops and the external_pending state in ONE store, which is safe only because the node is not shared yet.
    /// A separate post-construction state store would not fold across the atomic control word.
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
        static_assert(sizeof(async<T const, E>) == sizeof(impl::async_typed_node<T, E>),
                      "async<T const, E> must add no data members either — the view and the node are one object");
    }

    // compute frame
public:
    /// Install the compute frame, `async_step_status(async_context_base&)`, which resolves its outcome via the context.
    /// The frame closure wraps a typed async_context<T, E> around that base for the resolves.
    /// `f` is moved in once, here, and never moved again (see async_node_base's frame section).
    template <class F>
    void set_frame(F&& f)
    {
        set_frame_emplace<std::decay_t<F>>(cc::forward<F>(f));
    }

    /// Install the compute frame by building it in place from `args` — F never has to be movable at all.
    ///
    /// Call before the node is shared, as make_async_* does.
    /// Installing the frame is what determines F, and so which ops instance the node points at, and that pointer is written with a plain relaxed store.
    template <class F, class... Args>
    void set_frame_emplace(Args&&... args)
    {
        if constexpr (async::template frame_fits_inline<F>)
            this->template install_frame<async::frame_capacity, F>(&impl::async_type_ops_for_frame<T, E, F>,
                                                                   cc::forward<Args>(args)...);
        else
        {
            // too big for the inline slot: box it.
            // A cc::unique_function is one pointer, so IT fits inline, and create_from emplaces the closure into the box — an immovable F still works.
            using boxed_t = typename async::frame_type;
            this->template install_frame<async::frame_capacity, boxed_t>(
                &impl::async_type_ops_for_frame<T, E, boxed_t>,
                boxed_t::template create_from<F>(cc::default_node_allocator(), cc::forward<Args>(args)...));
        }
    }

    // owner-side access to the produced value
    //
    // Reading is on the view (try_value / value); these are the writing half, and they exist because an async is not a
    // shared-immutable: after completion one party is often the only one left, and moving a big payload out or editing it in
    // place beats copying it.
    //
    // What must hold is NO CONCURRENT ACCESS, which is weaker than sole ownership: two owners sequenced by their own
    // happens-before are fine, and that is why none of this asserts on the reference count.
    // What is NOT enough is readiness — finish_value publishes once, and a dependent that already read the value has no
    // reason to re-acquire, so a mutation racing a reader is a data race however ready the node is.
public:
    /// Pointer to the stored value for WRITING, or null unless ready with a value.
    /// See the section note: no concurrent access.
    [[nodiscard]] T* try_mutable_value() { return this->value_ptr(); }

    /// The stored value, for writing.
    /// Requires has_value().
    /// See the section note: no concurrent access.
    [[nodiscard]] T& mutable_value()
    {
        CC_ASSERT(this->has_value(), "async has no value (not ready, or ready with an error)");
        return *this->value_ptr();
    }

    /// MOVE the value out of the node.
    /// Requires has_value().
    ///
    /// The node stays ready_value over a moved-from husk, which teardown still destroys — so any OTHER live handle's later
    /// try_value() reads that husk.
    /// cc::into_result is the same operation over a handle it consumes.
    ///
    /// A copy-only T is silently COPIED rather than moved: T&& binds to T const&, so this leaves the stored value intact and
    /// "take" is then a lie.
    /// Nothing catches that but this sentence.
    [[nodiscard]] T take_value()
    {
        static_assert(std::is_move_constructible_v<T>, "take_value moves the value out of the node — T must be "
                                                       "move-constructible");
        CC_ASSERT(this->has_value(), "async has no value to take (not ready, or ready with an error)");
        return cc::move(*this->value_ptr());
    }

    /// MOVE the error out of the node.
    /// Requires has_error().
    ///
    /// Sharper than take_value, because the loss is SILENT: the node still reports has_error(), and every later
    /// propagate_error() on it — a dependent short-circuiting, try_async_blocking_get — hands back an async_error
    /// re-materialized from a message that is now empty.
    /// Take an error only from a node nothing else will ever read.
    [[nodiscard]] E take_error()
    {
        static_assert(std::is_move_constructible_v<E>, "take_error moves the error out of the node — E must be "
                                                       "move-constructible");
        CC_ASSERT(this->has_error(), "async has no error to take");
        return cc::move(*this->error_ptr());
    }

    // manual / promise-style completion (for externally produced values)
public:
    /// Complete externally with a value; wakes any parked dependents.
    /// Call at most once — unlike the resolve_* actions this is NOT asserted, and a second call runs teardown over a live value.
    void push_value(T v) { this->finish_value(cc::move(v)); } // builds the value into the payload, wakes dependents

    /// Complete externally with a value built in place from `args` (never moved) — the immovable-T path.
    template <class... Args>
    void push_value_emplace(Args&&... args)
    {
        this->template finish_value_emplace<T>(cc::forward<Args>(args)...);
    }

    /// Complete externally with an error; wakes any parked dependents.
    /// Call at most once, and see push_value: this is not asserted either.
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

/// The T/E-agnostic half of the compute-step context — what the poll loop and the type-erased stored frame name.
/// It reads dependencies and reports wait/yield; the typed async_context<T, E> wraps it to add the value/error resolves.
/// Not owned by the frame, and valid only for the duration of a single step.
struct cc::async_context_base
{
    async_node_base* current = nullptr;
    async_scheduler* scheduler = nullptr;

    // report-a-status helpers (no T/E needed)
public:
    [[nodiscard]] async_step_status wait_for_dependencies() const { return async_step_status::waiting; }
    [[nodiscard]] async_step_status yield() const { return async_step_status::yield; }

    // dependencies
public:
    /// Require an existing async as a dependency.
    /// Returns true if it is already ready, so read its value now.
    /// Otherwise it records a pending dependency and returns false, and the frame should return wait_for_dependencies().
    ///
    /// Neither subscribes nor schedules — the poll loop owns both, and scheduling here would enqueue a node we are about to run ourselves.
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

    // the step's resolution latch — read by async_node_base::invoke_frame_step's handler
public:
    /// True once this step resolved the node.
    /// A frame that throws AFTER resolving leaves nothing to fail: the frame is destroyed and the node may already be freed, so `current` is not even loadable.
    /// This is the only way the handler can tell that case apart from a frame that threw on its way to a result.
    [[nodiscard]] bool step_resolved() const { return _step->_resolved; }

private:
    template <class, class>
    friend struct cc::async_context; // the typed resolves latch through mark_step_resolved

    void mark_step_resolved() const { _step->_resolved = true; }

    /// The poll-stack context this step belongs to.
    /// The typed async_context<T, E> is built by COPYING this base, so the copy must not own the latch.
    /// The default member initializer points poll's own context at itself, and every copy inherits that pointer.
    /// Poll's context outlives the frame, so the latch stays readable after the node is gone.
    async_context_base* _step = this;
    bool _resolved = false;
};

/// The typed compute-step context the user's frame receives: `[](cc::async_context<T, E>& ctx) -> ...`.
/// It carries no extra state — a thin wrapper the frame closure builds around the async_context_base for the step, adding the value/error resolves that need T/E.
/// Inherits require / wait_for_dependencies / yield.
template <class T, class E>
struct cc::async_context : async_context_base
{
    /// Wrap the step's base context.
    /// Takes a non-const lvalue so the copied latch pointer cannot outlive a temporary — see async_context_base::_step.
    async_context(async_context_base& base) : async_context_base(base) {}

    // resolving the result — each returns the matching async_step_status, so a frame can `return ctx.xxx(...)`.
    // Resolving completes the node IN PLACE over the frame's own slot: it destroys the frame, the live executing closure, and builds the value/error there.
    // So resolve is terminal in the `delete this;` sense — the frame must not touch its captures, or the context, afterwards.
    // `return ctx.success(v);` is the shape.
public:
    /// Resolve with a value, moved into the payload.
    /// Anything convertible to T works, taken by value, so the conversion happens at the call site.
    /// What reaches the node is therefore a stack temporary of the payload's exact type, never a reference into a dependency's payload that the frame's captures pin alive.
    [[nodiscard]] async_step_status resolve_to_value(T v) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        this->mark_step_resolved();
        current->finish_value(cc::move(v)); // T exactly, so finish_value's decay deduces the payload type
        return async_step_status::produced_value;
    }

    /// Resolve with a value built in place from `args` (never moved) — the immovable-T path.
    ///
    /// `args` are forwarded by reference into the payload slot, and resolving destroys the frame first.
    /// So they must NOT reference the frame's own captures, nor anything only those captures keep alive, such as a dependency's value.
    /// Pass owned or stack-local arguments; resolve_to_value has no such caveat.
    template <class... Args>
    [[nodiscard]] async_step_status resolve_to_value_emplace(Args&&... args) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        this->mark_step_resolved();
        current->template finish_value_emplace<T>(cc::forward<Args>(args)...);
        return async_step_status::produced_value;
    }

    /// Resolve on the failure channel with E (moved into the payload).
    [[nodiscard]] async_step_status resolve_to_error(E e) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        this->mark_step_resolved();
        current->finish_error(cc::move(e));
        return async_step_status::produced_error;
    }

    /// Resolve on the failure channel with an error built in place from `args`.
    /// Same aliasing caveat as resolve_to_value_emplace: `args` must not reference the frame's captures.
    template <class... Args>
    [[nodiscard]] async_step_status resolve_to_error_emplace(Args&&... args) const
    {
        CC_ASSERT(!current->is_ready(), "this async already resolved — resolve exactly once");
        this->mark_step_resolved();
        current->template finish_error_emplace<E>(cc::forward<Args>(args)...);
        return async_step_status::produced_error;
    }

    // convenience aliases for readable frames
public:
    [[nodiscard]] async_step_status success(T v) const { return resolve_to_value(cc::move(v)); }
    [[nodiscard]] async_step_status error(E e) const { return resolve_to_error(cc::move(e)); }

    /// Only for the default failure channel: wrap a cc::any_error as an async_error.
    /// Enabled when E is async_error.
    [[nodiscard]] async_step_status error(cc::any_error e) const
        requires(std::is_same_v<E, async_error>)
    {
        return resolve_to_error(async_error::make_error(cc::move(e)));
    }
};

namespace cc
{

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

// A dependency argument is a shared_async<U, Ue>: awaited before f runs, then unwrapped to the stored U.
// Plain pass-through values are not supported yet — capture them in the closure.

template <class U, class Ue>
bool async_require_arg(async_context_base& ctx, shared_async<U, Ue> const& dep)
{
    return ctx.require(dep);
}

// Collect the first errored dep's error, propagated into THIS node's error type E (see async_error_propagate).
// The high-level sugar assumes a single failure type across the graph: a dep's propagated error, of its own Ue, must be constructible into E.
// For the default async_error everywhere, that is the identity.
template <class E, class U, class Ue>
void async_collect_arg_error(cc::optional<E>& out, shared_async<U, Ue> const& dep)
{
    if (!out.has_value() && dep->has_error())
        out.emplace_value(dep->propagate_error());
}

// returns a reference into the dependency node's stored value (stable while the node is alive)
// U is whatever the handle carries, so a shared_async<X const> dependency gives U = X const and this collapses to X const&.
template <class U, class Ue>
U const& async_unwrap_arg(shared_async<U, Ue> const& dep)
{
    return dep->value();
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

// invoke the user function and resolve the node with its result.
// A raw frame that itself returns a status — resolving via ctx and managing its own dependencies — is passed through unchanged.
// Any other return value is a plain value, resolved into the node here.
template <class T, class E, class F, class... Args>
async_step_status async_invoke_and_resolve(async_context<T, E>& ctx, F& f, Args&&... args)
{
    using r_t = std::remove_cvref_t<decltype(async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...))>;
    if constexpr (std::is_same_v<r_t, async_step_status>)
        return async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...);
    else
        return ctx.resolve_to_value(async_invoke_frame_fn(f, ctx, cc::forward<Args>(args)...));
}

// deduced value type of a value-returning frame f applied to the unwrapped dependency arguments, WITHOUT ctx.
// Deduction is context-free, because the typed context's own T is what we are deducing.
// So a value frame that also takes a ctx cannot be deduced, and must give the result type explicitly — as must every raw async_step_status frame.
template <class F, class... Deps>
using async_deduced_frame_result_t = std::remove_cvref_t<decltype(async_declval<std::decay_t<F>&>()(
    async_unwrap_arg(async_declval<std::decay_t<Deps> const&>())...))>;

// The node's value type: T verbatim, or the deduced frame result when T is the deduce sentinel.
// A partial specialization keeps the deduction LAZY: async_deduced_frame_result_t invokes f without a ctx, so it must not be instantiated when T is given explicitly.
// A raw ctx-resolving frame, which cannot be called ctx-free, is exactly that case.
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

// The one compute-frame wrapper behind every make_async_* form.
// Its stored signature is the type-erased async_context_base&.
// It builds the typed async_context<Result, E> for the step, requires all deps, short-circuits on the first error, then invokes f — passing the typed context only if f wants it.
// f's returned value is resolved here; a raw ctx-resolving frame's status is passed through.
// A raw state-machine frame is just the zero-dependency case where f takes async_context<Result, E>& and resolves and returns a status itself.
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

// The emplace twin of async_make_frame's lambda.
// That lambda captures `fn` by move, which an immovable F cannot do, so this holds F as a member built in place from the caller's args.
// No dependency arguments: the variadic dep form has to move its shared_async handles in anyway, so it stays on the lambda.
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

/// Allocate a node, and refuse a const T here rather than inside make_shared.
/// Every creation path goes through this — the make_async_* factories and the coroutine's get_return_object.
/// Without it, make_async_manual<int const>() reports a protected constructor from deep inside make_shared, instead of naming the fix.
template <class T, class E, class... Args>
[[nodiscard]] shared_async<T, E> async_new_node(Args&&... args)
{
    static_assert(!std::is_const_v<T>, "async<T const, E> is a read-only VIEW of async<T, E> — create the async<T, E> "
                                       "and convert");
    return cc::make_shared<async<T, E>, async_node_traits>(cc::forward<Args>(args)...);
}

// shared factory for make_async_*: build async<R, E> and install the wrapped frame
template <class T, class E, class F, class... Deps>
auto async_make_node(F&& f, Deps&&... deps)
{
    using result_t = async_result_type<T, F, Deps...>::type;
    static_assert(!std::is_void_v<result_t>, "the frame must return a value (wrap void as cc::unit)");
    static_assert(!std::is_same_v<result_t, async_step_status>,
                  "a raw async_context frame resolves via ctx and returns a status, not a value — give the "
                  "result type explicitly, e.g. make_async_lazy<int>(...)");

    auto node = async_new_node<result_t, E>();
    node->set_frame(async_make_frame<result_t, E>(cc::forward<F>(f), cc::forward<Deps>(deps)...));
    return node;
}
} // namespace impl

// ============================================================================
// creation
// ============================================================================

/// Create a cold (lazy) async — it runs only once scheduled, whether by another async requiring it or by async_blocking_get_singlethreaded driving it.
/// The compute frame is `f`, optionally followed by dependency arguments:
///
///   * each dependency is a shared_async<U>, awaited and unwrapped to the stored U before f runs.
///     Errors short-circuit: if any dependency failed, f is skipped and the first error propagates.
///   * f is called with the unwrapped dependency values.
///     It may take a leading async_context<T, E>&, for a raw state machine that manages its own dependencies, or omit it entirely — both are wrapped as needed.
///   * T defaults to the deduced return type of f, and deduction is context-free.
///     A value frame that also takes a ctx must give T explicitly, as must every raw async_context frame that returns a status.
///   * E is the failure channel, defaulting to async_error.
///
///   auto a = cc::make_async_scheduled<int>([](cc::async_context<int>&) { return 40; }); // raw frame
///   auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);                       // depends on a; f gets int
///   auto c = cc::make_async_lazy([] { return 7; });                                     // no deps, no context
template <class T = impl::async_deduce_result, class E = async_error, class F, class... Deps>
[[nodiscard]] auto make_async_lazy(F&& f, Deps&&... deps)
{
    return impl::async_make_node<T, E>(cc::forward<F>(f), cc::forward<Deps>(deps)...);
}

/// Like make_async_lazy, but builds the frame in place from `args` rather than moving a callable in, so an IMMOVABLE frame works.
/// The frame is never moved after installation either, not even when it parks.
///
/// T must be given explicitly: deducing it would mean invoking F, and F does not exist yet.
/// No dependency arguments — capture what the frame needs and require() it, or use make_async_lazy.
///
///   auto n = cc::make_async_lazy_emplace<int, cc::async_error, my_pinned_frame>(7);
template <class T, class E = async_error, class F, class... Args>
[[nodiscard]] shared_async<T, E> make_async_lazy_emplace(Args&&... args)
{
    auto node = impl::async_new_node<T, E>();
    node->template set_frame_emplace<impl::async_frame_holder<T, E, F>>(cc::forward<Args>(args)...);
    return node;
}

/// Like make_async_lazy, but eager: it schedules the node immediately, on the worker scope active here or else the installed default pool.
/// With neither, it stays cold and is scheduled when first required or driven.
/// Same forms as make_async_lazy.
template <class T = impl::async_deduce_result, class E = async_error, class F, class... Deps>
[[nodiscard]] auto make_async_scheduled(F&& f, Deps&&... deps)
{
    auto node = impl::async_make_node<T, E>(cc::forward<F>(f), cc::forward<Deps>(deps)...);
    if (impl::async_can_schedule_here())
        node->schedule();
    return node;
}

/// Create an async completed externally via async<T>::push_value / push_error — a promise-style node.
/// A dependent that requires it parks until it is pushed.
template <class T, class E = async_error>
[[nodiscard]] shared_async<T, E> make_async_manual()
{
    // The manual-tag ctor births the node external_pending in one store.
    return impl::async_new_node<T, E>(impl::async_manual_tag{});
}

// ============================================================================
// creation — born already ready (no frame, no scheduling)
// ============================================================================

/// An async that is already ready with `v` — no compute frame, no scheduling.
/// Usable immediately as a dependency, or read via try_value().
/// Moves `v` in, so T must be nothrow-move-constructible; use the _emplace form for an immovable T.
template <class T, class E = async_error>
[[nodiscard]] shared_async<T, E> make_async_from_value(T v)
{
    auto node = impl::async_new_node<T, E>();
    node->push_value(cc::move(v)); // cold node, empty unresolved arm -> drives straight to ready_value
    return node;
}

/// Like make_async_from_value, but builds the value in place from `args` (never moved) — the immovable-T path.
/// T is explicit, since args do not determine it: make_async_from_value_emplace<T>(args...).
template <class T, class E = async_error, class... Args>
[[nodiscard]] shared_async<T, E> make_async_from_value_emplace(Args&&... args)
{
    auto node = impl::async_new_node<T, E>();
    node->push_value_emplace(cc::forward<Args>(args)...);
    return node;
}

/// An async that is already ready on the failure channel with `e`.
/// T is explicit, since only E is determined by the argument: make_async_from_error<T>(e).
template <class T, class E = async_error>
[[nodiscard]] shared_async<T, E> make_async_from_error(E e)
{
    auto node = impl::async_new_node<T, E>();
    node->push_error(cc::move(e)); // cold node, empty unresolved arm -> drives straight to ready_error
    return node;
}

/// Like make_async_from_error, but builds the error in place from `args`.
/// T and E are explicit: make_async_from_error_emplace<T, E>(args...).
template <class T, class E = async_error, class... Args>
[[nodiscard]] shared_async<T, E> make_async_from_error_emplace(Args&&... args)
{
    auto node = impl::async_new_node<T, E>();
    node->push_error_emplace(cc::forward<Args>(args)...);
    return node;
}

// ============================================================================
// consuming — move the outcome out
// ============================================================================

/// Consume a READY async handle and MOVE its outcome out into a cc::result<T, E>: ready_value gives the moved value, ready_error gives cc::error(moved error).
/// Requires root to be ready, and takes the handle by value, consuming the caller's.
/// Unlike a getter this MOVES the payload out of the shared node, so any OTHER live handle's later try_value() / try_error() reads a moved-from value.
/// Use it only when you are done with the async.
/// T must be move-constructible; an immovable T is a compile error by design.
/// A read-only shared_async<T const, E> cannot be consumed at all — take_value / take_error are the owner's, and this is their handle-shaped spelling.
template <class T, class E = async_error>
[[nodiscard]] cc::result<T, E> into_result(shared_async<T, E> root)
{
    static_assert(!std::is_const_v<T>,
                  "into_result MOVES the outcome out, which a read-only async<T const, E> does not "
                  "allow — consume the async<T, E> instead");
    static_assert(std::is_move_constructible_v<T>, "into_result moves the value out of the node — T must be "
                                                   "move-constructible");
    CC_ASSERT(root != nullptr, "cannot consume a null async");
    CC_ASSERT(root->is_ready(), "into_result requires a ready async (drive it first)");

    if (root->has_error())
        return cc::error(root->take_error());
    return root->take_value();
}

// ============================================================================
// driving — a scheduler makes progress; blocking is its convenience
// ============================================================================

// You never block on an async — the ambient scheduler drives it, and these block the CALLING thread while that happens.
//
// **This is a bridge between the synchronous and the asynchronous world, and nothing more.**
// If you find yourself reaching for it often, step back: make the surrounding code async, write an ASYNC_TEST,
// return the async to a caller that can await it.
// A blocking_get inside a frame is legal — it participates rather than idling — but every blocked thread is one that
// cannot help, so overusing it is how a graph starves or deadlocks.

namespace impl
{
/// Drives `root` on the ambient scheduler until it is ready.
/// The scheduler returning early means it cannot progress — an unpushed manual node — so this waits and asks again.
void async_drive_until_ready(async_node_base& root);

/// async_drive_until_ready, giving up after `timeout_ms`. True if `root` is ready.
[[nodiscard]] bool async_drive_until_ready_for(async_node_base& root, i64 timeout_ms);
} // namespace impl

/// Drive `root` to completion on the ambient scheduler and return its outcome.
/// Ready on return, always: value or error, never still pending.
///
/// Requires an ambient scheduler (cc::install_default_async_scheduler, or a nexus run's).
/// Never returns while the graph is unfinished, so a node awaiting a push that never comes hangs here — that is the
/// honest failure, and try_async_blocking_get_for is the way out for a caller that cannot afford it.
///
/// A read-only shared_async<T const, E> works here: this copies rather than consumes, and the const is stripped from the
/// outcome because neither cc::result nor cc::optional can hold a const T.
template <class T, class E = async_error>
[[nodiscard]] cc::result<std::remove_const_t<T>, E> try_async_blocking_get(shared_async<T, E> const& root)
{
    using result_t = cc::result<std::remove_const_t<T>, E>;
    CC_ASSERT(root != nullptr, "cannot drive a null async");
    impl::async_drive_until_ready(*root);

    if (root->has_error())
        return result_t(cc::error(root->propagate_error()));
    return result_t(root->value()); // copy out
}

/// try_async_blocking_get, returning the value as a copy and asserting on error or cancellation.
/// For zero-copy access use root->try_value() after driving.
template <class T, class E = async_error>
[[nodiscard]] std::remove_const_t<T> async_blocking_get(shared_async<T, E> const& root)
{
    auto r = try_async_blocking_get<T, E>(root);
    CC_ASSERT(r.has_value(), "async completed with an error or was cancelled");
    return cc::move(r).value();
}

/// try_async_blocking_get, driven on `scheduler` rather than on the ambient one.
///
/// For code that owns a scheduler and means THAT one: a benchmark measuring a pool, a test standing one up.
/// Nullopt is "that scheduler could not finish it here" — the graph is parked on an external push, or another
/// scheduler already owns it — and the caller decides whether to push, wait, or try elsewhere.
template <class T, class E = async_error>
[[nodiscard]] cc::optional<cc::result<std::remove_const_t<T>, E>> try_async_blocking_get_on(async_scheduler& scheduler,
                                                                                            shared_async<T, E> const& root)
{
    using result_t = cc::result<std::remove_const_t<T>, E>;
    CC_ASSERT(root != nullptr, "cannot drive a null async");
    scheduler.participate_until_ready(*root);
    if (!root->is_ready())
        return cc::nullopt;

    if (root->has_error())
        return result_t(cc::error(root->propagate_error()));
    return result_t(root->value()); // copy out
}

/// try_async_blocking_get_on, returning the value as a copy and asserting on error, cancellation, or no progress.
template <class T, class E = async_error>
[[nodiscard]] std::remove_const_t<T> async_blocking_get_on(async_scheduler& scheduler, shared_async<T, E> const& root)
{
    auto r = try_async_blocking_get_on<T, E>(scheduler, root);
    CC_ASSERT(r.has_value(), "that scheduler could not complete the graph (parked on an external push, or owned by "
                             "another scheduler)");
    CC_ASSERT(r.value().has_value(), "async completed with an error or was cancelled");
    return cc::move(r).value().value();
}

/// try_async_blocking_get with a deadline: the outcome, or nullopt if `root` was still not ready after `timeout_ms`.
///
/// The one place "still pending" survives, for a caller that must not hang on a graph waiting for an external push.
/// It helps where it can — a scheduler bound here still runs work — but it never claims a slot to park in, so a
/// pool-driven graph simply continues on its own workers while this polls.
template <class T, class E = async_error>
[[nodiscard]] cc::optional<cc::result<std::remove_const_t<T>, E>> try_async_blocking_get_for(shared_async<T, E> const& root,
                                                                                             i64 timeout_ms)
{
    using result_t = cc::result<std::remove_const_t<T>, E>;
    CC_ASSERT(root != nullptr, "cannot drive a null async");
    if (!impl::async_drive_until_ready_for(*root, timeout_ms))
        return cc::nullopt;

    if (root->has_error())
        return result_t(cc::error(root->propagate_error()));
    return result_t(root->value()); // copy out
}
} // namespace cc
