#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>

namespace cnet::impl
{
struct io_operation;
class io_actor;
} // namespace cnet::impl

/// How an io_system is built.
struct cnet::io_system_description
{
    /// Run the reactor on the calling program's threads rather than one of our own.
    ///
    /// A build with `CC_HAS_THREADS == 0` behaves this way whatever this says, and so does wasm.
    /// It is worth setting deliberately in a test, where a reactor with no thread is what makes a run reproducible.
    bool unthreaded = false;

    /// What deadlines are measured against; null means the process-wide system clock.
    ///
    /// Must outlive the io_system.
    /// A test passes a `manual_clock` here and proves a timeout in microseconds instead of sleeping through it.
    clock* time_source = nullptr;

    /// The longest one wait parks for, in milliseconds.
    ///
    /// A ceiling rather than a cadence: submitting or cancelling an operation wakes the reactor at once, so this only
    /// bounds how long an otherwise idle reactor takes to notice something it was not told about.
    i32 max_wait_ms = 50;
};

/// The reactor, and the thread it may or may not have.
///
/// **There is no `poll()` here, on purpose.**
/// With threads, this owns one and nothing is asked of the caller.
/// Without them, it registers a pump with `cc::register_thread_pump`, so every blocking wait anywhere in the process
/// sweeps it -- and a caller that drives `cc::thread_pump_all()` for the rest of shaped-core drives this too.
/// A `cnet`-specific pump would be exactly the deadlock the registry exists to prevent: a wait that drains only what
/// one library can name stalls the moment the thing it waits for lives somewhere else.
///
/// **Nothing here requires blocking to obtain a result.**
/// That is what keeps a browser main thread and a render thread first-class callers, neither of which may block.
class cnet::io_system
{
public:
    /// Build one, with or without sockets under it.
    ///
    /// A platform with no sockets -- wasm -- gets a system that runs timers and manual operations and nothing else,
    /// because a deadline still has to fire where an HTTP request is served by `fetch`.
    /// It is always unthreaded there, since a reactor with nothing to wait on cannot park.
    [[nodiscard]] static cc::result<cc::unique_ptr<io_system>, error> try_create(io_system_description const& desc = {});

    /// Throwing counterpart of try_create.
    [[nodiscard]] static cc::unique_ptr<io_system> create(io_system_description const& desc = {});

    /// Whether this system has a thread of its own.
    ///
    /// A caller that drives `cc::thread_pump_all()` regardless is correct either way; this is for diagnostics and for
    /// a test that wants to assert which mode it got.
    [[nodiscard]] bool has_reactor_thread() const;

    /// The clock deadlines are measured against.
    [[nodiscard]] clock& time_source() const;

    /// Hand an operation to the reactor.
    ///
    /// **For the transport layer**, not for callers of this library.
    /// Safe from any thread: the operation crosses to the reactor through the actor's mailbox, and the reactor is
    /// woken so it does not sit on a wait it could have ended.
    /// The operation must stay alive until its `on_complete` has run.
    void submit(impl::io_operation* op);

    /// Ask for an operation to finish as `cancelled`.
    ///
    /// **For the transport layer.** Safe from any thread, and harmless if the operation already completed.
    void cancel(impl::io_operation* op);

    /// Say that a `manual` operation is done, so it completes successfully.
    ///
    /// **For the transport layer**, and the reason a backend that is not a socket -- a virtual connection, a browser
    /// `fetch` -- can join the same async machinery.
    /// Safe from any thread, and harmless if the operation already completed.
    void signal(impl::io_operation* op);

    /// How many operations are outstanding.
    ///
    /// A snapshot rather than a promise: it can change the instant it is read, and it is here for diagnostics and for
    /// a test asserting that nothing was left behind.
    [[nodiscard]] isize pending_count() const;

    io_system() = default;
    io_system(io_system const&) = delete;
    io_system& operator=(io_system const&) = delete;
    ~io_system();

private:
    /// Held by pointer so threaded_actor.hh stays out of this header: it reaches MSVC's <xutility> and the whole
    /// AVX-512 intrinsic surface behind it, which is most of that header's parse time.
    cc::unique_ptr<impl::io_actor> _actor;
};

namespace cnet
{
/// The absolute reading of the io_system's clock a deadline turns into, or 0 for none.
///
/// **For the transport layer.** A relative deadline is what a call site can write honestly, and an absolute one is
/// what the reactor can compare against -- and this is the one place the two meet, so every transport measures the
/// same budget against the same clock.
[[nodiscard]] i64 deadline_to_absolute(io_system& io, deadline d);
} // namespace cnet
