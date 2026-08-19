#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread_pump.hh>

// COST NOTE: the STL headers below reach MSVC's <xutility>, which pulls <immintrin.h>.
// That is the whole AVX-512 intrinsic surface — ~43 extra files, and most of this header's parse time.
// <memory>, <string>, <string_view>, <mutex>, <system_error>, <ranges> and <chrono> all reach it.
// <type_traits>, <utility> and <atomic> do not, and are cheap by comparison.
// So keeping one of the first group out of a widely-included header is worth real time.
// docs/notes/build-times.md has the measurement and the per-header table.
#include <concepts>
#include <condition_variable>
#include <memory>
#include <thread>
#include <type_traits>

// cc::threaded_actor: an actor with its own thread and a typed message mailbox.
// Messages are moved into the mailbox and processed one at a time, in strict global send order, on a private thread.
// Actor-local state therefore needs no locks, and blocking (a GPU wait, say) only stalls this one actor.
//
// Use it for a long-lived, stateful subsystem such as uploads or IO.
// Not for heavy CPU fan-out — cc::async and its thread pool are for that.
//
// Derive from cc::threaded_actor_impl<Msg...>, override on_message(Msg) for each type, then build via
// cc::make_threaded_actor<YourImpl>(args...).
// start() spawns the thread; shutdown() drains and joins, and the destructor joins too.
//
//   struct log_line { cc::string text; };
//   class logger : public cc::threaded_actor_impl<log_line>
//   {
//       cc::vector<cc::string> _log;
//   protected:
//       void on_message(log_line msg) override { _log.push_back(cc::move(msg.text)); }
//   };
//
//   auto actor = cc::make_and_start_threaded_actor<logger>();
//   actor->enqueue_message(log_line{"hello"});
//   actor->shutdown();
//
// Threading modes are chosen at start().
// By default an actor runs its own thread where the platform has them (threaded_actor_mode::threaded_if_possible).
// Pass threaded_actor_mode::unthreaded — the only real option on single-threaded platforms like WebAssembly —
// to run without a thread.
// Nothing is spawned, and you drive processing yourself with process_messages_if_unthreaded[_for_ms](), which
// is a no-op when a thread is running and so is safe to call unconditionally.
// That keeps one code path across platforms, and makes tests deterministic and race-free.
// Caveat: unthreaded, on_message and the hooks run on the *calling* thread, so a blocking handler stalls it.
// The "blocking only stalls this actor" property holds in threaded mode only.
//
// Note: messages are stored in a cc::variant<Msg...>, which enforces that the message types are pairwise distinct.
// The polymorphic impl is held by std::unique_ptr, because cc::unique_ptr forbids the upcast/downcast this needs.

/// How a threaded_actor is driven, chosen at start().
enum class cc::threaded_actor_mode
{
    /// run the actor on its own thread where the platform has threads, else fall back to unthreaded
    threaded_if_possible,
    /// run no thread; drive processing via process_messages_if_unthreaded[_for_ms]()
    unthreaded,
};

namespace cc
{

// CRTP mixins (plumbing, not user-facing); the aggregate types live in fwd.hh
template <class MessageT>
struct threaded_actor_message_handler;
template <class MessageT, class ActorT>
struct threaded_actor_enqueue_message;
} // namespace cc

/// Type-erased threading and lifecycle layer shared by every threaded_actor<Msg...>.
/// Owns the thread, the shutdown flags and the inbox condition variable; the typed actor supplies message dispatch through drain_inbox_messages.
struct cc::threaded_actor_base
{
    // lifecycle
public:
    /// Starts the actor; call exactly once.
    /// In threaded mode this spawns the actor thread; unthreaded it runs no thread, and you must drive it with process_messages_if_unthreaded[_for_ms]().
    /// Messages enqueued before start() are kept.
    /// On single-threaded platforms the mode is forced to unthreaded regardless of the argument.
    void start(threaded_actor_mode mode = threaded_actor_mode::threaded_if_possible);

    /// Requests shutdown without blocking; new messages are rejected immediately.
    /// Lets several actors begin shutting down in parallel before any join.
    /// Call at most once, after start() and before shutdown() — shutdown() calls it for you if you did not.
    void begin_shutdown();

    /// Requests shutdown and blocks until all queued messages have been drained.
    /// In threaded mode it joins the thread; unthreaded, it drains synchronously on the caller.
    /// Runs on_thread_shutdown before returning, and must be called at most once, after start().
    void shutdown();

    // manual pump (only meaningful when started unthreaded)
public:
    /// Runs one processing cycle on the calling thread: drain the inbox, dispatch, one on_process.
    /// Returns true if there may be more work — something was dispatched, or on_process asked to run again.
    /// A no-op returning false unless started unthreaded and not shut down, so it is safe to call unconditionally every frame.
    bool process_messages_if_unthreaded();

    /// Repeats process_messages_if_unthreaded() until idle or max_ms of wall-clock elapses; max_ms <= 0 runs a single cycle.
    /// Returns true if it stopped on the budget with work still pending.
    bool process_messages_if_unthreaded_for_ms(double max_ms);

    /// True once shutdown has begun: new messages are rejected and the thread is winding down.
    [[nodiscard]] bool is_shutting_down() const;
    /// True once shutdown has finished and the thread has joined.
    [[nodiscard]] bool is_shut_down() const;
    /// True while started and not yet shutting down: enqueue_message will be accepted.
    [[nodiscard]] bool is_running() const;

    virtual ~threaded_actor_base() = default;

    // supplied by the typed actor
protected:
    [[nodiscard]] virtual threaded_actor_impl_base& get_impl() = 0;

    /// Logs a swallowed exception from a context that must not propagate (thread body, destructor).
    static void report_unhandled_exception(char const* where) noexcept;

    // internal
private:
    /// Drains pending inbox messages and dispatches them to on_message.
    /// When wait_on_cond_var is true, blocks until messages arrive or shutdown begins.
    /// Returns true if any were dispatched.
    virtual bool drain_inbox_messages(bool wait_on_cond_var) = 0;

    /// Actor thread entry point: on_thread_init, then a loop of drain + on_process until shutdown drains the
    /// inbox empty, then on_thread_shutdown.
    void execute_actor_thread();

    // members
private:
    std::thread _thread;
    cc::thread_pump_registration _pump_registration; // held only while unthreaded: this actor as a semantic thread
    std::condition_variable _inbox_cond_var;         // one wake point shared by all message types
    cc::atomic<bool> _is_started = false;
    cc::atomic<bool> _is_shutting_down = false;
    cc::atomic<bool> _is_shut_down = false;
    bool _is_unthreaded = false; // set once at start(); no background thread touches it

    template <class... MessageT>
    friend struct threaded_actor;
};

/// Base for actor implementations: lifecycle hooks that all run on the actor thread.
/// Derive from threaded_actor_impl<Msg...>, which adds the typed on_message handlers, rather than from this directly.
struct cc::threaded_actor_impl_base
{
    virtual ~threaded_actor_impl_base() = default;

    /// Label used as the OS thread name.
    /// Must outlive the actor thread — a string literal, or a member that lives at least as long.
    [[nodiscard]] virtual cc::string_view actor_name() const noexcept { return "threaded_actor"; }

    /// Runs on the actor thread before any message, for thread-local setup.
    virtual void on_thread_init() {}

    /// Runs on the actor thread after the loop exits; no messages follow.
    /// The impl outlives the thread, so actor-local state is safe to touch here.
    virtual void on_thread_shutdown() {}

    /// Called each loop after draining the inbox, even when it was empty.
    /// Return true to be called again immediately, for instance to work through a local queue; false to sleep until new messages arrive.
    /// Spurious wakeups during shutdown are safe.
    virtual bool on_process() { return false; }
};

/// One typed on_message(Msg) handler; threaded_actor_impl mixes in one per message type.
template <class MessageT>
struct cc::threaded_actor_message_handler
{
protected:
    /// Called on the actor thread for each MessageT, in global send order across all message types.
    virtual void on_message(MessageT msg) = 0;
};

/// Derive from this and override on_message(Msg) per message type; the header comment shows the shape.
/// Actor-local state lives here and needs no locks, since every hook runs on the actor thread.
/// MessageT... must be movable.
template <class... MessageT>
struct cc::threaded_actor_impl : threaded_actor_impl_base, threaded_actor_message_handler<MessageT>...
{
    static_assert(sizeof...(MessageT) > 0, "a threaded_actor must accept at least one message type");

    using threaded_actor_t = threaded_actor<MessageT...>;

    // expose every on_message(Msg) as a single overload set for variant dispatch
    using threaded_actor_message_handler<MessageT>::on_message...;
};

/// One typed enqueue_message(Msg); threaded_actor mixes in one per message type (CRTP), forming an overload
/// set that forwards into the actor's private queue.
template <class MessageT, class ActorT>
struct cc::threaded_actor_enqueue_message
{
    /// Enqueue a message; see threaded_actor::enqueue_message for semantics.
    bool enqueue_message(MessageT msg) { return static_cast<ActorT*>(this)->impl_enqueue_message(cc::move(msg)); }
};

/// Handle for an actor, built with cc::make_threaded_actor<YourImpl>(...) — you do not derive from this.
/// It owns the impl, so actor-local state cannot be touched cross-thread and the destructor shuts down cleanly.
/// Lifecycle misuse, such as bad start/shutdown ordering, asserts.
template <class... MessageT>
struct cc::threaded_actor final : threaded_actor_base,
                                  threaded_actor_enqueue_message<MessageT, threaded_actor<MessageT...>>...
{
    static_assert(sizeof...(MessageT) > 0, "a threaded_actor must accept at least one message type");

    /// Enqueues a message for the actor thread; returns false if the actor is shutting down, in which case the message is rejected.
    /// All types share one global send order, and every message sent before begin_shutdown is processed before the thread exits.
    using threaded_actor_enqueue_message<MessageT, threaded_actor<MessageT...>>::enqueue_message...;

    // ctor
public:
    explicit threaded_actor(std::unique_ptr<threaded_actor_impl<MessageT...>> impl) : _impl(cc::move(impl))
    {
        CC_ASSERT(_impl != nullptr, "actor impl must not be null");
    }

    /// Reclaims the impl after the actor has fully shut down, to read out results.
    /// Asserts the actor is shut down and that ActorImplT is the actual impl type; the handle is unusable afterwards.
    template <std::derived_from<threaded_actor_impl_base> ActorImplT>
    [[nodiscard]] std::unique_ptr<ActorImplT> take_impl()
    {
        CC_ASSERT(this->is_shut_down(), "cannot take the impl while the actor is still running");
        CC_ASSERT(_impl != nullptr, "impl was already taken");

        auto* derived = dynamic_cast<ActorImplT*>(_impl.get());
        CC_ASSERT(derived != nullptr, "impl has a different type than requested");

        _impl.release(); // ownership moves to the returned pointer
        return std::unique_ptr<ActorImplT>(derived);
    }

    /// Joins the actor thread if still running.
    /// Must live here, not in ~threaded_actor_base, so _impl outlives on_thread_shutdown.
    /// Exceptions cannot escape a destructor, so they are logged.
    ~threaded_actor() override
    {
        if (this->is_running())
        {
            try
            {
                this->shutdown();
            }
            catch (...)
            {
                report_unhandled_exception("destructor shutdown");
            }
        }
    }

    // internal
private:
    threaded_actor_impl_base& get_impl() override { return *_impl; }

    // store the message as a variant under lock, then wake the thread
    template <class T>
    bool impl_enqueue_message(T msg)
    {
        if (this->_is_shutting_down.load())
            return false;

        this->_inbox.lock([&](cc::vector<cc::variant<MessageT...>>& queue) { queue.emplace_back(cc::move(msg)); });
        this->_inbox_cond_var.notify_one();
        return true;
    }

    bool drain_inbox_messages(bool wait_on_cond_var) override
    {
        auto const move_into_local = [&](cc::vector<cc::variant<MessageT...>>& queue)
        {
            for (auto& msg : queue)
                _local_inbox.push_back(cc::move(msg));
            queue.clear(); // keep capacity
        };

        if (wait_on_cond_var)
            this->_inbox.wait(
                this->_inbox_cond_var, [&](cc::vector<cc::variant<MessageT...>> const& queue)
                { return !queue.empty() || this->is_shutting_down(); }, move_into_local);
        else
            this->_inbox.lock(move_into_local);

        if (_local_inbox.empty())
            return false;

        // dispatch each message to the matching on_message(T) via overload resolution
        for (auto& msg : _local_inbox)
            cc::move(msg).visit([&](auto&& alt) { _impl->on_message(cc::move(alt)); });

        _local_inbox.clear(); // keep capacity
        return true;
    }

    // members
private:
    /// Globally ordered inbox; one queue for all types preserves cross-type ordering.
    cc::mutex<cc::vector<cc::variant<MessageT...>>> _inbox;

    /// Thread-local staging drained from _inbox, keeping the lock hold short.
    cc::vector<cc::variant<MessageT...>> _local_inbox;

    /// std::unique_ptr, not cc::unique_ptr: ownership is polymorphic through the impl base.
    std::unique_ptr<threaded_actor_impl<MessageT...>> _impl;

    template <class, class>
    friend struct threaded_actor_enqueue_message;
};

namespace cc
{
/// Builds a YourImpl and wraps it in its threaded_actor<Msg...> handle, deduced from YourImpl::threaded_actor_t.
/// The impl type does not leak into the return type.
/// Call start() to begin processing; YourImpl must derive from cc::threaded_actor_impl<Msg...>.
template <class ActorImplT, class... Args>
[[nodiscard]] auto make_threaded_actor(Args&&... args)
{
    static_assert(std::is_base_of_v<threaded_actor_impl_base, ActorImplT>, "the actor implementation must derive from "
                                                                           "cc::threaded_actor_impl<...>");

    using actor_t = typename ActorImplT::threaded_actor_t;
    return cc::make_unique<actor_t>(std::make_unique<ActorImplT>(cc::forward<Args>(args)...));
}

/// Like make_threaded_actor, but also calls start(); returns a running actor handle.
template <class ActorImplT, class... Args>
[[nodiscard]] auto make_and_start_threaded_actor(Args&&... args)
{
    auto actor = cc::make_threaded_actor<ActorImplT>(cc::forward<Args>(args)...);
    actor->start();
    return actor;
}
} // namespace cc
