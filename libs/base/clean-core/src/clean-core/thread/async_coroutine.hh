#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/thread/async.hh>

#include <coroutine>

// ============================================================================
// co_await / co_return for cc::async
// ============================================================================
//
// A coroutine IS a compute frame.
// The frame contract — re-entrant, polled repeatedly, never moved, resolving through async_context — is exactly a coroutine's shape, so this layer adds no node state at all.
// The node stores one coroutine_handle, so a coroutine's frame is 8 B and never falls back to the boxed cc::unique_function.
//
// A graph that does not use co_await pays nothing: no field, no poll-loop branch, and <coroutine> stays out of async.hh.
// Including THIS header is what makes a function returning shared_async<T, E> a coroutine.
//
//   cc::shared_async<int> load(cc::string p)      // eager: scheduled at its initial suspend
//   {
//       auto const& bytes = co_await read(p);     // short-circuits if read(p) failed
//       co_return parse(bytes);
//   }
//
// co_await never STARTS work — creation does.
// require() is a wakeup edge; parallelism comes from a node being scheduled and stealable.
// So `co_await a; co_await b;` serializes nothing as long as a and b were created eagerly, which is what the default gives you.
// cc::async_all is for the other case, where the fan-out is built at the await site.
//
// Parameters are captured by their DECLARED type, so a reference parameter dangles across the first suspend.
// Take coroutine parameters by value.
//
// T must be movable here, since co_return moves the result through the promise.
// An immovable T stays on the raw-frame emplace API.

/// The return type of a LAZY coroutine — the escape hatch from eager scheduling.
/// The node is born cold, exactly like make_async_lazy, and runs only once something requires or schedules it.
/// Eager scheduling has to happen at the initial suspend, which is over before the caller sees the handle, so the choice lives in the return type.
template <class T, class E> // the default E lives on the fwd.hh declaration
struct cc::async_lazy
{
    cc::shared_async<T, E> node;

    operator cc::shared_async<T, E>() const { return node; }
    [[nodiscard]] cc::async<T, E>* operator->() const { return node.get(); }
    [[nodiscard]] explicit operator bool() const { return node != nullptr; }
};

namespace cc::impl
{
template <class...>
inline constexpr bool async_coro_always_false = false;

/// Type-erased "may the coroutine be resumed?" check, run by the frame BEFORE it resumes.
/// Returns false when it wrote the promise's failure slot instead, which short-circuits the rest of the body.
using async_coro_check_fn = bool (*)(void* awaiter, void* promise);

/// What a suspended coroutine shares with the frame driving it.
/// Split out of the promise so an awaiter can reach it without naming the coroutine's T.
template <class T, class E>
struct async_promise_state
{
    /// The node this coroutine resolves — raw, and deliberately so.
    /// The node owns the coroutine through its frame, so an owning back-pointer would be a cycle.
    cc::async<T, E>* node = nullptr;

    /// The step context, valid ONLY while the frame is inside one poll.
    /// An awaiter reaches require() through it, which is why co_await outside a compute step is an assert rather than a hang.
    cc::async_context_base* ctx = nullptr;

    /// The awaiter suspended right now, and the check it wants run before the next resume.
    /// A null check means "just resume": a yield, or an awaiter that deliberately does not short-circuit on a failed dependency.
    void* awaiter = nullptr;
    async_coro_check_fn check = nullptr;

    /// The ONE failure slot, written by a dependency short-circuit, an escaped exception, or cc::async_fail.
    /// The frame reads it, destroys the coroutine and resolves the node on the failure channel.
    cc::optional<E> failure;

    /// Set by cc::async_yield for the current step only; the frame clears it before every resume.
    bool yielded = false;

    void suspend_on(void* a, async_coro_check_fn fn)
    {
        awaiter = a;
        check = fn;
    }

    /// Write the failure slot, converting `x` into this coroutine's failure channel E.
    /// The first failure wins, matching the make_async_* sugar.
    /// The conversion ladder is E-from-x, then the default channel's any_error wrap — the same single-failure-type assumption the sugar makes.
    template <class X>
    void fail_with(X&& x, cc::source_location site = cc::source_location::current())
    {
        if (failure.has_value())
            return;

        if constexpr (std::is_constructible_v<E, X&&>)
            failure.emplace_value(E(cc::forward<X>(x)));
        else if constexpr (std::is_same_v<E, cc::async_error> //
                           && std::is_constructible_v<cc::any_error, X&&, cc::source_location>)
            failure.emplace_value(cc::async_error::make_error(cc::any_error(cc::forward<X>(x), site)));
        else
            static_assert(async_coro_always_false<X>,
                          "cannot build this coroutine's failure channel E from the given error — the async sugar "
                          "assumes a single E across a graph");
    }
};

/// The node's stored compute frame for a coroutine: one handle, so it is 8 B and always inline.
/// It OWNS the coroutine, which is what reclaims an abandoned or never-scheduled one when the node is torn down.
template <class P>
struct async_coro_frame
{
    std::coroutine_handle<P> handle;

    explicit async_coro_frame(std::coroutine_handle<P> h) : handle(h) {}

    async_coro_frame(async_coro_frame&& r) noexcept : handle(r.handle) { r.handle = {}; }
    async_coro_frame(async_coro_frame const&) = delete;
    async_coro_frame& operator=(async_coro_frame&&) = delete;
    async_coro_frame& operator=(async_coro_frame const&) = delete;

    ~async_coro_frame()
    {
        if (handle)
            handle.destroy();
    }

    async_step_status operator()(async_context_base& base)
    {
        using T = typename P::value_type;
        using E = typename P::error_type;

        auto& p = handle.promise();
        p.ctx = &base;
        p.yielded = false;

        auto resume = true;
        if (p.check != nullptr)
        {
            auto* const a = p.awaiter;
            auto* const fn = p.check;
            p.awaiter = nullptr;
            p.check = nullptr;
            resume = fn(a, &p); // a failed dependency writes the failure slot here and refuses the resume
        }
        if (resume)
            handle.resume();
        p.ctx = nullptr;

        auto ctx = async_context<T, E>(base);

        // Both terminal paths read the promise BEFORE destroying the coroutine, and drop the handle before resolving.
        if (p.failure.has_value())
        {
            auto e = cc::move(p.failure.value());
            destroy_coroutine();
            return ctx.resolve_to_error(cc::move(e));
        }
        if (handle.done())
        {
            auto v = p.take_value();
            destroy_coroutine();
            return ctx.resolve_to_value(cc::move(v));
        }

        return p.yielded ? base.yield() : base.wait_for_dependencies();
    }

private:
    /// Destroy the coroutine and clear the handle, which must happen BEFORE the resolve that follows.
    /// Resolving destroys this frame, and ~async_coro_frame would otherwise destroy the coroutine a second time.
    void destroy_coroutine()
    {
        auto const h = handle;
        handle = {};
        h.destroy();
    }
};

/// The co_return half of the promise, for a value-producing coroutine.
/// return_void and return_value cannot coexist, which is why the cc::unit specialization below loses `co_return cc::error(...)`.
template <class T, class E, bool IsUnit>
struct async_promise_return : async_promise_state<T, E>
{
    cc::optional<T> result;

    void return_value(T v) { result.emplace_value(cc::move(v)); }

    template <class F>
    void return_value(cc::as_error_t<F>&& e)
    {
        auto const site = e.site();
        this->fail_with(cc::move(e).take_error(), site);
    }

    [[nodiscard]] T take_value() { return cc::move(result.value()); }
};

/// A cc::unit coroutine: bare `co_return;`, and cc::async_fail for the failure path.
template <class T, class E>
struct async_promise_return<T, E, true> : async_promise_state<T, E>
{
    void return_void() {}

    [[nodiscard]] cc::unit take_value() { return {}; }
};

template <class T, class E, bool Eager>
struct async_promise : async_promise_return<T, E, std::is_same_v<T, cc::unit>>
{
    using value_type = T;
    using error_type = E;

    // The coroutine frame is the one allocation this layer adds over a lambda frame, so it goes to the same slab the node does.
    // node_allocation_free is stateless, which matters here: a coroutine is routinely destroyed on a different thread than the one that created it.
    static constexpr isize frame_alignment = 16;

    [[nodiscard]] static void* operator new(std::size_t n)
    {
        auto const idx = cc::node_class_index_from_size_and_align(isize(n), frame_alignment);
        return cc::default_node_allocator().allocate_node_bytes(idx, isize(n), frame_alignment);
    }
    static void operator delete(void* p, std::size_t n)
    {
        auto const idx = cc::node_class_index_from_size_and_align(isize(n), frame_alignment);
        cc::node_allocation_free(static_cast<byte*>(p), idx);
    }

    /// Build the node and install this coroutine as its frame.
    /// Runs before initial_suspend, and the node is unshared until we return it, so nothing can poll a coroutine that is still inside its ramp.
    [[nodiscard]] auto get_return_object()
    {
        auto node = cc::make_shared<cc::async<T, E>, impl::async_node_traits>();
        this->node = node.get();
        node->set_frame(async_coro_frame<async_promise>(std::coroutine_handle<async_promise>::from_promise(*this)));

        if constexpr (Eager)
            return node;
        else
            return cc::async_lazy<T, E>{cc::move(node)};
    }

    struct initial_awaiter
    {
        cc::async<T, E>* node;

        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept
        {
            if constexpr (Eager)
            {
                // The coroutine is fully suspended here, so publishing the node is safe — which is why this cannot live in get_return_object.
                // Nothing may touch the coroutine after the schedule: a peer can steal, run and destroy it before this returns.
                if (cc::async_scheduler::current_or_null() != nullptr)
                    node->schedule();
            }
        }
        void await_resume() const noexcept {}
    };

    [[nodiscard]] initial_awaiter initial_suspend() noexcept { return {this->node}; }

    /// Always suspend: the frame reads the result out of the promise and destroys the coroutine itself.
    [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }

    /// Contain the exception here rather than letting it cross resume(), so it never reaches the poll loop's own handler.
    /// The policy is the frame API's: a channel that declares no mapping is a runtime diagnostic, never a compile error.
    void unhandled_exception()
    {
        if constexpr (impl::async_error_from_exception_capable<E>)
            this->failure.emplace_value(
                cc::custom::async_error_from_exception_trait<E>::make(impl::async_describe_current_exception()));
        else
        {
            CC_ASSERT(false, "exception escaped a coroutine whose failure channel declares no mapping from one");
            throw;
        }
    }
};

// ============================================================================
// awaiters
// ============================================================================

/// Await one dependency, short-circuiting on failure — the plain `co_await a`.
template <class U, class Ue>
struct async_awaiter_value
{
    cc::shared_async<U, Ue> const* dep;

    [[nodiscard]] bool await_ready() const { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        auto& p = h.promise();
        CC_ASSERT(p.ctx != nullptr, "co_await outside of a compute step");

        if ((*dep)->has_error())
        {
            p.fail_with((*dep)->propagate_error());
            return true; // stay suspended: the frame destroys us and propagates
        }
        if (p.ctx->require(*dep))
            return false; // already ready — no suspension at all

        p.suspend_on(this, &check<P>);
        return true;
    }

    /// A reference INTO the dependency node's payload, so reading it copies nothing.
    /// It stays valid only while a handle keeps that node alive — binding `auto const&` to the result of awaiting a temporary dangles.
    [[nodiscard]] U const& await_resume() const { return *(*dep)->value_ptr(); }

private:
    template <class P>
    static bool check(void* self, void* promise)
    {
        auto const* const a = static_cast<async_awaiter_value const*>(self);
        auto& p = *static_cast<P*>(promise);
        if ((*a->dep)->has_error())
        {
            p.fail_with((*a->dep)->propagate_error());
            return false;
        }
        return true;
    }
};

/// Await one dependency WITHOUT short-circuiting: the coroutine is resumed either way and inspects the outcome itself.
/// `AsResult` picks what await_resume hands back — nothing, or a cc::result built from the settled node.
template <class U, class Ue, bool AsResult>
struct async_awaiter_settled
{
    cc::shared_async<U, Ue> const* dep;

    [[nodiscard]] bool await_ready() const { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        auto& p = h.promise();
        CC_ASSERT(p.ctx != nullptr, "co_await outside of a compute step");
        return !p.ctx->require(*dep); // no check installed: a failed dependency still resumes us
    }

    [[nodiscard]] decltype(auto) await_resume() const
    {
        if constexpr (AsResult)
        {
            static_assert(std::is_copy_constructible_v<U>, "async_as_result copies the value out — use "
                                                           "cc::async_settled and read the node in place");
            if ((*dep)->has_error())
                return cc::result<U, Ue>(cc::error((*dep)->propagate_error()));
            return cc::result<U, Ue>(*(*dep)->value_ptr());
        }
    }
};

/// Await a pack of dependencies at once.
/// Every one is required BEFORE the decision to park, so a fan-out parks once on all of them rather than walking them in sequence.
template <class... Deps>
struct async_awaiter_all
{
    cc::tuple<Deps const*...> deps;

    [[nodiscard]] bool await_ready() const { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        auto& p = h.promise();
        CC_ASSERT(p.ctx != nullptr, "co_await outside of a compute step");

        auto all_ready = true;
        cc::apply([&](auto const*... ds) { ((all_ready = p.ctx->require(*ds) && all_ready), ...); }, deps);

        if (!collect_failure(p))
            return true; // a dependency already failed: stay suspended and let the frame propagate
        if (all_ready)
            return false;

        p.suspend_on(this, &check<P>);
        return true;
    }

    void await_resume() const {}

private:
    /// False once a failed dependency has written the promise's failure slot.
    template <class P>
    bool collect_failure(P& p) const
    {
        auto ok = true;
        cc::apply(
            [&](auto const*... ds)
            {
                auto const one = [&](auto const& d)
                {
                    if (ok && d->has_error())
                    {
                        p.fail_with(d->propagate_error());
                        ok = false;
                    }
                };
                (one(*ds), ...);
            },
            deps);
        return ok;
    }

    template <class P>
    static bool check(void* self, void* promise)
    {
        return static_cast<async_awaiter_all const*>(self)->collect_failure(*static_cast<P*>(promise));
    }
};

/// The dynamic-fan-out twin of async_awaiter_all, over a span of same-typed dependencies.
template <class U, class Ue>
struct async_awaiter_all_span
{
    cc::span<cc::shared_async<U, Ue> const> deps;

    [[nodiscard]] bool await_ready() const { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        auto& p = h.promise();
        CC_ASSERT(p.ctx != nullptr, "co_await outside of a compute step");

        auto all_ready = true;
        for (auto const& d : deps)
            all_ready = p.ctx->require(d) && all_ready;

        if (!collect_failure(p))
            return true;
        if (all_ready)
            return false;

        p.suspend_on(this, &check<P>);
        return true;
    }

    void await_resume() const {}

private:
    template <class P>
    bool collect_failure(P& p) const
    {
        for (auto const& d : deps)
            if (d->has_error())
            {
                p.fail_with(d->propagate_error());
                return false;
            }
        return true;
    }

    template <class P>
    static bool check(void* self, void* promise)
    {
        return static_cast<async_awaiter_all_span const*>(self)->collect_failure(*static_cast<P*>(promise));
    }
};

struct async_awaiter_yield
{
    [[nodiscard]] bool await_ready() const { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) const
    {
        h.promise().yielded = true;
        return true;
    }

    void await_resume() const {}
};

/// Fail the coroutine's node without unwinding: it is never resumed, and the frame destroys it while suspended.
template <class X>
struct async_awaiter_fail
{
    X value;
    cc::source_location site;

    [[nodiscard]] bool await_ready() const { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        h.promise().fail_with(cc::move(value), site);
        return true;
    }

    void await_resume() const {}
};
} // namespace cc::impl

namespace cc
{
/// Await a dependency and read its value.
/// A failed dependency SHORT-CIRCUITS: the rest of the body never runs, its in-scope locals are destroyed, and the failure propagates to this node.
/// Use async_as_result or async_settled to handle the failure instead.
template <class U, class Ue>
[[nodiscard]] impl::async_awaiter_value<U, Ue> operator co_await(shared_async<U, Ue> const& dep)
{
    return {&dep};
}

/// Await a dependency without short-circuiting, and read the outcome off the node yourself.
/// Copies nothing, and works for a move-only or immovable U.
template <class U, class Ue>
[[nodiscard]] impl::async_awaiter_settled<U, Ue, false> async_settled(shared_async<U, Ue> const& dep)
{
    return {&dep};
}

/// Await a dependency and get its outcome as a value, instead of short-circuiting on failure.
/// Copies the value out, so a move-only or large U wants async_settled.
template <class U, class Ue>
[[nodiscard]] impl::async_awaiter_settled<U, Ue, true> async_as_result(shared_async<U, Ue> const& dep)
{
    return {&dep};
}

/// Await several dependencies at once, requiring all of them before parking.
/// It hands back nothing: read each value with a plain `co_await`, which no longer suspends once the node is ready.
///
///   co_await cc::async_all(a, b, c);
///   auto const sum = co_await a + co_await b;
///
/// This is the spelling for a fan-out built AT the await site.
/// Dependencies created eagerly need no combinator — they are already in flight, so awaiting them one by one costs no concurrency.
template <class... Deps>
[[nodiscard]] impl::async_awaiter_all<Deps...> async_all(Deps const&... deps)
{
    return {{&deps...}};
}

template <class U, class Ue>
[[nodiscard]] impl::async_awaiter_all_span<U, Ue> async_all(cc::span<shared_async<U, Ue> const> deps)
{
    return {deps};
}

/// Yield cooperatively: the node is rescheduled and the coroutine resumes on a later poll.
[[nodiscard]] inline impl::async_awaiter_yield async_yield()
{
    return {};
}

/// Fail this coroutine's node.
/// The uniform failure spelling — a cc::unit coroutine has no `co_return cc::error(...)`, because return_void and return_value cannot coexist.
template <class X>
[[nodiscard]] impl::async_awaiter_fail<std::decay_t<X>> async_fail(X&& x,
                                                                   cc::source_location s = cc::source_location::current())
{
    return {cc::forward<X>(x), s};
}

template <size_t N>
[[nodiscard]] impl::async_awaiter_fail<cc::string> async_fail(char const (&str)[N],
                                                              cc::source_location s = cc::source_location::current())
{
    return {cc::string(str, isize(N - 1)), s};
}
} // namespace cc

/// A coroutine returning shared_async<T, E> is EAGER: scheduled at its initial suspend if a worker scope is bound, exactly like make_async_scheduled.
template <class T, class E, class... Args>
struct std::coroutine_traits<cc::shared_ptr<cc::async<T, E>, cc::impl::async_node_traits>, Args...>
{
    using promise_type = cc::impl::async_promise<T, E, true>;
};

/// A coroutine returning async_lazy<T, E> is COLD until something requires or schedules it.
template <class T, class E, class... Args>
struct std::coroutine_traits<cc::async_lazy<T, E>, Args...>
{
    using promise_type = cc::impl::async_promise<T, E, false>;
};
