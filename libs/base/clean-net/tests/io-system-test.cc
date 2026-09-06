#include <clean-core/common/macros.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/impl/native_socket.hh>
#include <clean-net/impl/reactor.hh>
#include <clean-net/io/io_system.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// The reactor with an owner: threaded, and driven by the repo-wide pump when it is not.
// Everything is loopback, so nothing leaves the machine.

namespace
{
/// An operation whose completion is readable from another thread, since in threaded mode it lands on the reactor's.
struct signal_op : impl::io_operation
{
    cc::atomic<bool> completed = false;
    cc::atomic<bool> failed = false;
    error_code code = error_code::unknown;

    void on_complete(cc::optional<error> failure) override
    {
        if (failure.has_value())
        {
            code = failure.value().code;
            failed.store(true);
        }
        completed.store(true);
    }
};

/// Closes on every exit path, and movable so a fixture can hand one back.
struct socket_guard
{
    impl::native_socket handle = impl::k_invalid_socket;

    socket_guard() = default;
    explicit socket_guard(impl::native_socket s) : handle(s) {}

    socket_guard(socket_guard&& rhs) noexcept : handle(rhs.handle) { rhs.handle = impl::k_invalid_socket; }
    socket_guard& operator=(socket_guard&& rhs) noexcept
    {
        if (this != &rhs)
        {
            impl::close_socket(handle);
            handle = rhs.handle;
            rhs.handle = impl::k_invalid_socket;
        }
        return *this;
    }

    socket_guard(socket_guard const&) = delete;
    socket_guard& operator=(socket_guard const&) = delete;
    ~socket_guard() { impl::close_socket(handle); }
};

/// Wait for `done` without ever asking cnet to pump.
///
/// `cc::thread_pump_all()` is the whole point: it is what a threads-off application already calls for the rest of
/// shaped-core, and it drives the reactor with no cnet-specific call anywhere.
/// With a real reactor thread it sweeps an empty registry and costs one atomic load.
///
/// The budget is wall-clock rather than a round count, and generously so.
/// A round count measures how fast this machine spins rather than how long the reactor was given, and a debug build
/// burns through thousands of yields in the time a threaded handoff takes -- which is a flaky test rather than a
/// found bug.
/// Five seconds against an expected handful of milliseconds still fails a reactor that has genuinely stalled.
bool wait_for(cc::function_ref<bool()> done, f64 budget_secs = 5.0)
{
    auto& clk = system_clock();
    auto const deadline_ns = clk.now_ns() + i64(budget_secs * 1e9);

    while (true)
    {
        if (done())
            return true;
        if (clk.now_ns() >= deadline_ns)
            return false;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
}

/// A listening socket on loopback, and the endpoint to reach it at.
struct listener_fixture
{
    socket_guard socket;
    endpoint where;

    [[nodiscard]] static cc::optional<listener_fixture> make()
    {
        auto created = impl::create_tcp_socket(ip_family::v4);
        if (created.has_error())
            return {};

        auto fixture = listener_fixture();
        fixture.socket.handle = created.value();
        if (impl::bind_socket(fixture.socket.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_error())
            return {};
        if (impl::listen_socket(fixture.socket.handle, 8).has_error())
            return {};

        auto local = impl::local_endpoint(fixture.socket.handle);
        if (local.has_error())
            return {};
        fixture.where = local.value();
        return cc::optional<listener_fixture>(cc::move(fixture));
    }
};

/// One connect and one accept against a fresh listener, driven through `io`.
///
/// False when this platform has no sockets to make one from, which the caller turns into a skip: an io_system exists
/// there anyway, because a request served by `fetch` still has a deadline.
[[nodiscard]] bool check_connect_and_accept(io_system& io)
{
    auto listener = listener_fixture::make();
    if (!listener.has_value())
        return false;

    auto accept_op = signal_op();
    accept_op.kind = impl::io_op_kind::accept;
    accept_op.socket = listener.value().socket.handle;
    io.submit(&accept_op);

    auto client_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(client_created.has_value());
    auto const client = socket_guard(client_created.value());

    auto connect_op = signal_op();
    connect_op.kind = impl::io_op_kind::connect;
    connect_op.socket = client.handle;
    connect_op.peer = listener.value().where;
    io.submit(&connect_op);

    CHECK(wait_for([&] { return accept_op.completed.load() && connect_op.completed.load(); }));
    CHECK(!connect_op.failed.load());
    CHECK(!accept_op.failed.load());
    CHECK(accept_op.accepted != impl::k_invalid_socket);
    impl::close_socket(accept_op.accepted);

    // Nothing is left outstanding, which is the property a leaked operation would break.
    CHECK(wait_for([&] { return io.pending_count() == 0; }));
    return true;
}
} // namespace

TEST("cnet - an io_system comes up and reports which mode it got")
{
    auto io = io_system::try_create();
    if (io.has_error())
    {
        // The only reason to fail is a platform with no sockets.
        CHECK(io.error().code == error_code::unsupported);
        SKIP("this platform has no sockets");
    }

    // A build without threads is unthreaded whatever the description says, and so is one with no sockets to poll:
    // io_system::create gates on both, and a reactor thread with nothing pollable behind it would only spin.
    // Both terms are needed because wasm separates them -- it has no sockets either way, but it does have threads
    // once built with -pthread, so a threading-only expectation is right there for the wrong reason.
    CHECK(io.value()->has_reactor_thread() == (CC_HAS_THREADS != 0 && impl::sockets_are_supported()));
    CHECK(io.value()->pending_count() == 0);
    CHECK(&io.value()->time_source() == &system_clock());
}

TEST("cnet - the io_system carries a connect and an accept to completion")
{
    auto io = io_system::try_create();
    if (io.has_error())
        SKIP("this platform has no sockets");

    if (!check_connect_and_accept(*io.value()))
        SKIP("this platform has no sockets");
}

TEST("cnet - an unthreaded io_system is driven by the repo-wide pump alone")
{
    // The mode a threads-off build and wasm always get, reproduced on a native host so it is debuggable here.
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    CHECK(!io.value()->has_reactor_thread());

    // Nothing below calls into cnet to make progress: cc::thread_pump_all() is the only driver, and the actor
    // registered itself with it.
    // A cnet-specific pump would be the deadlock that registry exists to prevent.
    if (!check_connect_and_accept(*io.value()))
        SKIP("this platform has no sockets");
}

TEST("cnet - an unthreaded reactor starts and stays idle with nothing submitted")
{
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    // The pump contract -- report progress only when there was some -- is asserted against the reactor directly in
    // reactor-test.cc, and cannot be asserted here: cc::thread_pump_all() is process-wide, nexus runs tests in
    // parallel, and another test's io_system registered in the same registry would answer for this one.
    CHECK(io.value()->pending_count() == 0);
    CHECK(!io.value()->has_reactor_thread());

    // Sweeping is safe whether or not anything is registered, which is what makes an unconditional pump correct.
    (void)cc::thread_pump_all();
    CHECK(io.value()->pending_count() == 0);
}

TEST("cnet - a deadline is measured against the clock the io_system was given")
{
    auto clk = manual_clock(0);
    auto io = io_system::try_create({.unthreaded = true, .time_source = &clk});
    if (io.has_error())
        SKIP("this platform has no sockets");

    CHECK(&io.value()->time_source() == &clk);

    auto created = impl::create_udp_socket(ip_family::v4);
    if (created.has_error())
        SKIP("this platform has no sockets");
    auto const s = socket_guard(created.value());
    CHECK(impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());

    char inbox[8] = {};
    auto receive_op = signal_op();
    receive_op.kind = impl::io_op_kind::receive;
    receive_op.socket = s.handle;
    receive_op.buffer = reinterpret_cast<byte*>(inbox);
    receive_op.buffer_size = isize(sizeof(inbox));
    receive_op.deadline_ns = 5ll * 1000 * 1000 * 1000; // five seconds by that clock, and nobody will ever send
    io.value()->submit(&receive_op);

    // The message has to reach the reactor before the count means anything.
    CHECK(wait_for([&] { return io.value()->pending_count() == 1; }));
    CHECK(!receive_op.completed.load());

    // Five seconds pass in no time at all, which is the whole reason the clock is a seam.
    clk.advance_ms(5001);
    CHECK(wait_for([&] { return receive_op.completed.load(); }));
    CHECK(receive_op.code == error_code::timed_out);
}

TEST("cnet - cancelling through the io_system completes the operation as cancelled")
{
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    auto created = impl::create_udp_socket(ip_family::v4);
    if (created.has_error())
        SKIP("this platform has no sockets");
    auto const s = socket_guard(created.value());
    CHECK(impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());

    char inbox[8] = {};
    auto receive_op = signal_op();
    receive_op.kind = impl::io_op_kind::receive;
    receive_op.socket = s.handle;
    receive_op.buffer = reinterpret_cast<byte*>(inbox);
    receive_op.buffer_size = isize(sizeof(inbox));
    io.value()->submit(&receive_op);

    CHECK(wait_for([&] { return io.value()->pending_count() == 1; }));

    io.value()->cancel(&receive_op);
    CHECK(wait_for([&] { return receive_op.completed.load(); }));
    CHECK(receive_op.code == error_code::cancelled);
    CHECK(wait_for([&] { return io.value()->pending_count() == 0; }));
}
