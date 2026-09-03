#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/impl/native_socket.hh>

/// The reactor: what waits on many sockets at once, and finishes the work that was waiting on them.
///
/// **Completion-shaped, not readiness-shaped.**
/// A caller submits an operation and is told when it finished, never when a socket became ready.
/// That is the harder shape to retrofit, so it is the one this commits to: a readiness poller can present
/// completions by doing the read itself, while a completion port cannot be bent into reporting readiness.
/// It is also what keeps the layers above free of `EWOULDBLOCK`, partial writes and the retry loops they imply.
///
/// One concrete class rather than an interface with per-platform subclasses, following `sr::window_system`: the
/// implementation is swapped by compiling a different `poll_once`, and everything else here is platform-agnostic.
///
/// **Every method except wake() runs on the reactor thread.**
/// The io_system actor above is what serializes user calls onto it, so nothing here takes a lock.
/// `wake()` is the one exception and is safe from any thread.

namespace cnet::impl
{
/// What an operation is waiting to finish.
enum class io_op_kind : u8
{
    /// An outbound connection.
    /// Completes when the handshake finished or the peer refused.
    connect,

    /// One inbound connection off a listening socket.
    /// Completes with `accepted` set.
    accept,

    /// Bytes into `buffer`.
    /// Completes on the FIRST bytes that arrive, which may be fewer than asked for -- a stream has no message
    /// boundaries, so waiting for a full buffer would be waiting for a message nobody sent.
    receive,

    /// Bytes out of `buffer`.
    /// Completes only when ALL of them are gone, since a partial send is never what a caller meant and the retry
    /// loop is the reactor's to run.
    send,

    /// Nothing at all, until the clock passes `deadline_ns`.
    /// The one operation whose deadline is a SUCCESS rather than a failure, which is what makes it a delay: a retry
    /// backoff, a happy-eyeballs head start, the latency a simulated link adds.
    timer,

    /// Nothing until somebody calls `signal`, and `timed_out` if nobody does.
    ///
    /// How work that is not a socket joins the same async machinery: a virtual connection handing bytes to its peer,
    /// and later a browser `fetch` reporting back.
    /// Signalling is the only way to complete one, so all of its state stays on the reactor thread and needs no lock.
    manual,
};

/// One outstanding operation.
///
/// Owned by whoever submitted it, and it must stay alive until `on_complete` has run.
/// `on_complete` runs exactly once, on the reactor thread, and the operation is no longer known to the reactor by
/// the time it does -- so a handler may free it, and may submit another.
struct io_operation
{
    io_op_kind kind = io_op_kind::receive;
    native_socket socket = k_invalid_socket;

    /// receive: where the bytes land, send: the bytes to write.
    /// Unused otherwise.
    byte* buffer = nullptr;
    isize buffer_size = 0;

    /// connect: where to connect to.
    endpoint peer;

    /// accept: the connection that arrived, owned by the handler from then on.
    native_socket accepted = k_invalid_socket;

    /// An absolute reading of the reactor's clock; 0 means no deadline.
    /// The operation fails with `timed_out` once the clock passes it, whether or not the socket ever became ready.
    i64 deadline_ns = 0;

    /// How many bytes moved.
    /// Meaningful for `receive` and `send`.
    isize transferred = 0;

    /// Called exactly once, on the reactor thread; `failure` is absent on success.
    virtual void on_complete(cc::optional<error> failure) = 0;

    io_operation() = default;
    io_operation(io_operation const&) = delete;
    io_operation& operator=(io_operation const&) = delete;
    virtual ~io_operation() = default;
};

class reactor
{
public:
    /// Build a reactor, with or without sockets under it.
    ///
    /// A build with no sockets gets one that runs timers and manual operations and nothing else, which is what keeps
    /// deadlines working on wasm -- where the transport is absent but an HTTP request still has a budget.
    /// The clock is the seam deadlines are measured against, and it must outlive the reactor.
    /// Taking one here rather than reading the OS is what makes a timeout testable without sleeping through it.
    [[nodiscard]] static cc::result<cc::unique_ptr<reactor>, error> try_create(clock& c);

    /// Hand an operation over.
    /// It never completes inline -- always from a later `wait`.
    void submit(io_operation* op);

    /// Ask for `op` to finish with `cancelled` at the next opportunity.
    /// Harmless if it already completed, which is what makes a cancel racing a completion safe rather than a bug.
    void cancel(io_operation* op);

    /// Say that a `manual` operation is done, so it completes successfully at the next opportunity.
    /// Harmless if it already completed, and meaningless on any other kind.
    void signal(io_operation* op);

    /// Wait up to `timeout_ms` for something to happen, then finish whatever is ready or overdue.
    ///
    /// Returns the number of operations completed.
    /// `timeout_ms` of 0 never blocks, which is the only legal value on a browser main thread and the one an
    /// unthreaded pump uses; a negative value waits without limit.
    /// A pending deadline shortens the wait, so a timeout fires on time even when no socket ever becomes ready.
    [[nodiscard]] i32 wait(i32 timeout_ms);

    /// Make a thread blocked in `wait` return promptly.
    /// **The only method here callable from another thread.**
    void wake();

    /// How many operations are outstanding, for diagnostics and for the "nothing left to do" question.
    [[nodiscard]] isize pending_count() const { return _pending.size(); }

    explicit reactor(clock& c, native_socket wake_socket) : _clock(c), _wake_socket(wake_socket) {}
    reactor(reactor const&) = delete;
    reactor& operator=(reactor const&) = delete;
    ~reactor();

private:
    struct entry
    {
        io_operation* op = nullptr;
        bool cancelled = false;
        bool signalled = false;
        bool readable = false;
        bool writable = false;
        bool errored = false;

        /// A failure discovered at submit time, delivered on the next wait so nothing ever completes inline.
        cc::optional<error> immediate_failure;
    };

    struct completion
    {
        io_operation* op = nullptr;
        cc::optional<error> failure;
    };

    /// The most sockets one wait can watch, which `select`'s `FD_SETSIZE` fixes on Windows.
    /// Platform-specific, like `poll_once` itself.
    [[nodiscard]] static isize max_watched();

    /// Ask the OS what is ready, and record it on the entries.
    /// Platform-specific, along with `drive_socket`, `wake` and `drain_wake`; everything else here is not.
    void poll_once(i32 timeout_ms);

    /// Swallow the wake bytes and re-arm.
    void drain_wake();

    [[nodiscard]] i32 clamp_timeout(i32 timeout_ms) const;
    [[nodiscard]] i32 complete_ready();

    /// Move one operation as far as it can go.
    /// Absent means "still pending"; present means it finished, with or without a failure.
    [[nodiscard]] cc::optional<cc::optional<error>> drive(entry& e);

    /// The half of `drive` that talks to a socket, and the only half a build without sockets does not have.
    [[nodiscard]] cc::optional<cc::optional<error>> drive_socket(entry& e);

    /// The self-wake channel, or `k_invalid_socket` where the platform has no sockets to build one from.
    [[nodiscard]] static cc::result<native_socket, error> create_wake_channel();

    clock& _clock;
    native_socket _wake_socket = k_invalid_socket;
    cc::atomic<bool> _wake_pending = false;
    cc::vector<entry> _pending;

    /// Where the next wait starts watching from.
    ///
    /// Past `max_watched()` operations a wait cannot watch them all, and always starting at the front would mean the
    /// tail is never watched at all -- a connection that is starved rather than slow.
    /// So each wait resumes where the last one stopped.
    /// It is an index into a vector that changes between waits, which makes this approximate: the property it buys is
    /// that no operation is systematically skipped, not that turns are exactly fair.
    isize _watch_cursor = 0;
};
} // namespace cnet::impl
