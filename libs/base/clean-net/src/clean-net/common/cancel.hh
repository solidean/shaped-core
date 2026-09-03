#pragma once

#include <clean-net/fwd.hh>

namespace cnet::impl
{
struct io_operation;
class cancel_state;

// The control block is opaque on purpose: it holds a cc::mutex, whose header reaches MSVC's <xutility> and the whole
// AVX-512 intrinsic surface behind it.
// A cc::shared_ptr member would need the complete type at every copy and destruction, and so would drag all of that
// into every translation unit that includes tcp.hh.
// So the refcount is intrusive and these two are the only thing the header knows about it.
void cancel_state_retain(cancel_state* s);
void cancel_state_release(cancel_state* s);
[[nodiscard]] cancel_state* cancel_state_create();
[[nodiscard]] cancel_state* cancel_state_create_child(cancel_state* parent);
} // namespace cnet::impl

/// Ask the operations of a whole request to stop.
///
/// A deadline is how an operation ends early when the world is slow.
/// This is how it ends early because the program changed its mind -- a server shutting down, a user pressing stop, a
/// speculative connection that lost its race.
///
/// **A token groups, and a deadline bounds.**
/// One token covers the resolve, the connect, the handshake and every read of one request, so cancelling the request
/// cancels all of them, while each of those still carries its own deadline.
/// They are deliberately two parameters rather than one budget: for a plain connect they are the same grouping, and
/// above it they are not -- an HTTP client wants a per-read timeout inside a request-wide cancel.
///
/// **Cancelling does not stop anything now.** The socket may be mid-write.
/// The promise it keeps is the reactor's own: the operation completes with `error_code::cancelled` at the next
/// opportunity, and no later work happens on its behalf.
/// Callers branch on `cc::async_error::is_cancelled()`, which is where that outcome already had a home -- a cancelled
/// read that reported zero bytes would be indistinguishable from a peer that hung up.
///
/// Copyable, and safe to use from any thread.
/// The default token is empty and allocates nothing, which is what the vast majority of calls -- the ones that never
/// cancel -- pass.
class cnet::cancel_token
{
public:
    /// A token nothing can be cancelled through, and the default every call takes.
    cancel_token() = default;

    /// A token with a control block behind it, which is what makes it able to cancel.
    [[nodiscard]] static cancel_token create();

    /// A token this one cancels, and that can also be cancelled on its own.
    ///
    /// **This is how a token composes downward.**
    /// An operation made of several smaller ones -- a connect that races two addresses, a request that resolves,
    /// connects and reads -- gives each part a child, so cancelling the request cancels every part while finishing
    /// the race cancels only its losers.
    /// A child of an empty token is simply an independent token, since there is nothing above it to answer to.
    [[nodiscard]] cancel_token create_child() const;

    /// Whether there is anything behind this token at all.
    [[nodiscard]] bool is_valid() const { return _state != nullptr; }

    /// Whether `cancel` has been called.
    /// False for the default token, which can never be cancelled.
    [[nodiscard]] bool is_cancelled() const;

    /// Ask every operation registered here, now or later, to finish as `cancelled`.
    ///
    /// Idempotent, and safe from any thread.
    /// It applies to operations started afterwards too: a token, once cancelled, stays cancelled, so a late caller
    /// fails at once rather than starting work nobody wants.
    void cancel() const;

    /// The control block, for the transport layer to register operations against.
    [[nodiscard]] impl::cancel_state* state() const { return _state; }

    cancel_token(cancel_token const& o) : _state(o._state) { impl::cancel_state_retain(_state); }
    cancel_token(cancel_token&& o) noexcept : _state(o._state) { o._state = nullptr; }
    cancel_token& operator=(cancel_token const& o);
    cancel_token& operator=(cancel_token&& o) noexcept;
    ~cancel_token() { impl::cancel_state_release(_state); }

private:
    impl::cancel_state* _state = nullptr;
};

namespace cnet::impl
{
/// Keeps an operation registered with a token for exactly as long as the operation exists.
///
/// **Attach after submitting, not before.**
/// A cancel arriving in between would otherwise be posted ahead of the operation it means to cancel, so `attach`
/// re-reads the token afterwards and cancels the operation itself if it has to.
///
/// **Detach first thing in `on_complete`**, so a cancel racing that completion finds a registration that is still
/// alive rather than an operation that has already freed itself.
struct cancel_registration
{
    void attach(cancel_token const& token, io_system& io, io_operation* op);
    void detach();

    cancel_registration() = default;
    cancel_registration(cancel_registration const&) = delete;
    cancel_registration& operator=(cancel_registration const&) = delete;
    ~cancel_registration() { detach(); }

private:
    cancel_state* _state = nullptr;
    io_operation* _op = nullptr;
};
} // namespace cnet::impl
