#include <clean-core/function/function_ref.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/impl/reactor.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// The reactor, driven by hand: no actor, no thread, and every wait explicit.
// Everything here is loopback, so nothing leaves the machine and nothing outside the process has to be running.

namespace
{
/// Records the one completion an operation ever gets.
struct capture_op : impl::io_operation
{
    bool completed = false;
    cc::optional<error> failure;

    void on_complete(cc::optional<error> f) override
    {
        CC_ASSERT(!completed, "an operation completed twice");
        completed = true;
        failure = cc::move(f);
    }

    [[nodiscard]] bool succeeded() const { return completed && !failure.has_value(); }
    [[nodiscard]] error_code code() const { return failure.has_value() ? failure.value().code : error_code::unknown; }
};

struct socket_guard
{
    impl::native_socket handle = impl::k_invalid_socket;

    socket_guard() = default;
    explicit socket_guard(impl::native_socket s) : handle(s) {}
    socket_guard(socket_guard const&) = delete;
    socket_guard& operator=(socket_guard const&) = delete;
    ~socket_guard() { impl::close_socket(handle); }
};

/// Drive the reactor until `done` holds, or give up.
///
/// A bounded budget rather than a spin: a reactor that never completes an operation should fail the test rather than
/// hang the suite, and a hung suite is the one failure mode nobody can diagnose from CI.
bool pump_until(impl::reactor& r, cc::function_ref<bool()> done, i32 max_waits = 500)
{
    for (i32 i = 0; i < max_waits; ++i)
    {
        if (done())
            return true;
        (void)r.wait(10);
    }
    return done();
}

/// A port nothing is listening on: bind one, learn its number, then give it back.
cc::optional<endpoint> closed_port()
{
    auto created = impl::create_tcp_socket(ip_family::v4);
    if (created.has_error())
        return {};

    auto const s = socket_guard(created.value());
    if (impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), false).has_error())
        return {};

    auto local = impl::local_endpoint(s.handle);
    if (local.has_error())
        return {};
    return local.value();
}
} // namespace

TEST("cnet - the reactor completes a loopback connect and accept")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto& clk = system_clock();
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    auto listener_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(listener_created.has_value());
    auto const listener = socket_guard(listener_created.value());
    CHECK(impl::bind_socket(listener.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());
    CHECK(impl::listen_socket(listener.handle, 8).has_value());

    auto const where = impl::local_endpoint(listener.handle);
    CHECK(where.has_value());

    auto accept_op = capture_op();
    accept_op.kind = impl::io_op_kind::accept;
    accept_op.socket = listener.handle;
    r.submit(&accept_op);

    auto client_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(client_created.has_value());
    auto const client = socket_guard(client_created.value());

    auto connect_op = capture_op();
    connect_op.kind = impl::io_op_kind::connect;
    connect_op.socket = client.handle;
    connect_op.peer = where.value();
    r.submit(&connect_op);

    CHECK(pump_until(r, [&] { return accept_op.completed && connect_op.completed; }));
    CHECK(connect_op.succeeded());
    CHECK(accept_op.succeeded());
    CHECK(accept_op.accepted != impl::k_invalid_socket);

    auto const accepted = socket_guard(accept_op.accepted);

    // The accepted side knows who connected, and it is the client's own address.
    auto const peer = impl::remote_endpoint(accepted.handle);
    CHECK(peer.has_value());
    CHECK(peer.value().address.is_loopback());

    // Nothing is left outstanding once both finished.
    CHECK(r.pending_count() == 0);
}

TEST("cnet - the reactor moves bytes both ways")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto& clk = system_clock();
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    auto listener_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(listener_created.has_value());
    auto const listener = socket_guard(listener_created.value());
    CHECK(impl::bind_socket(listener.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());
    CHECK(impl::listen_socket(listener.handle, 8).has_value());
    auto const where = impl::local_endpoint(listener.handle).value();

    auto accept_op = capture_op();
    accept_op.kind = impl::io_op_kind::accept;
    accept_op.socket = listener.handle;
    r.submit(&accept_op);

    auto client_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(client_created.has_value());
    auto const client = socket_guard(client_created.value());

    auto connect_op = capture_op();
    connect_op.kind = impl::io_op_kind::connect;
    connect_op.socket = client.handle;
    connect_op.peer = where;
    r.submit(&connect_op);

    CHECK(pump_until(r, [&] { return accept_op.completed && connect_op.completed; }));
    CHECK(accept_op.succeeded());
    auto const accepted = socket_guard(accept_op.accepted);

    // client -> server
    char const greeting[] = "hello reactor";
    auto send_op = capture_op();
    send_op.kind = impl::io_op_kind::send;
    send_op.socket = client.handle;
    send_op.buffer = reinterpret_cast<byte*>(const_cast<char*>(greeting));
    send_op.buffer_size = isize(sizeof(greeting) - 1);
    r.submit(&send_op);

    char inbox[64] = {};
    auto receive_op = capture_op();
    receive_op.kind = impl::io_op_kind::receive;
    receive_op.socket = accepted.handle;
    receive_op.buffer = reinterpret_cast<byte*>(inbox);
    receive_op.buffer_size = isize(sizeof(inbox));
    r.submit(&receive_op);

    CHECK(pump_until(r, [&] { return send_op.completed && receive_op.completed; }));
    CHECK(send_op.succeeded());
    CHECK(receive_op.succeeded());

    // A send completes only when every byte is gone, so this is the whole message.
    CHECK(send_op.transferred == isize(sizeof(greeting) - 1));
    CHECK(receive_op.transferred == isize(sizeof(greeting) - 1));
    CHECK(cc::string_view(inbox, receive_op.transferred) == "hello reactor");
}

TEST("cnet - a closed peer reads as connection_closed rather than an empty success")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto& clk = system_clock();
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    auto listener_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(listener_created.has_value());
    auto const listener = socket_guard(listener_created.value());
    CHECK(impl::bind_socket(listener.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());
    CHECK(impl::listen_socket(listener.handle, 8).has_value());
    auto const where = impl::local_endpoint(listener.handle).value();

    auto accept_op = capture_op();
    accept_op.kind = impl::io_op_kind::accept;
    accept_op.socket = listener.handle;
    r.submit(&accept_op);

    auto client_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(client_created.has_value());
    auto client = socket_guard(client_created.value());

    auto connect_op = capture_op();
    connect_op.kind = impl::io_op_kind::connect;
    connect_op.socket = client.handle;
    connect_op.peer = where;
    r.submit(&connect_op);

    CHECK(pump_until(r, [&] { return accept_op.completed && connect_op.completed; }));
    auto const accepted = socket_guard(accept_op.accepted);

    // The client hangs up without sending anything.
    impl::close_socket(client.handle);
    client.handle = impl::k_invalid_socket;

    char inbox[16] = {};
    auto receive_op = capture_op();
    receive_op.kind = impl::io_op_kind::receive;
    receive_op.socket = accepted.handle;
    receive_op.buffer = reinterpret_cast<byte*>(inbox);
    receive_op.buffer_size = isize(sizeof(inbox));
    r.submit(&receive_op);

    CHECK(pump_until(r, [&] { return receive_op.completed; }));
    CHECK(receive_op.failure.has_value());

    // A clean hang-up and a reset are different things, and only one of them is anybody's fault.
    // Which one a platform reports for a peer that closed without sending is its own business.
    auto const hung_up
        = receive_op.code() == error_code::connection_closed || receive_op.code() == error_code::connection_reset;
    CHECK(hung_up);
}

TEST("cnet - a refused connection fails with connection_refused")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto const closed = closed_port();
    CHECK(closed.has_value());

    auto& clk = system_clock();
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    auto client_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(client_created.has_value());
    auto const client = socket_guard(client_created.value());

    auto connect_op = capture_op();
    connect_op.kind = impl::io_op_kind::connect;
    connect_op.socket = client.handle;
    connect_op.peer = closed.value();
    r.submit(&connect_op);

    CHECK(pump_until(r, [&] { return connect_op.completed; }));
    CHECK(connect_op.failure.has_value());

    // This is the case WSAPoll cannot report, and the reason the Windows poller is select.
    CHECK(connect_op.code() == error_code::connection_refused);
}

TEST("cnet - a deadline fires without anything having to happen on the socket")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    // A clock a test can move is what makes this run in microseconds rather than in whatever the timeout says.
    auto clk = manual_clock(1000);
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    auto created = impl::create_udp_socket(ip_family::v4);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());
    CHECK(impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());

    char inbox[8] = {};
    auto receive_op = capture_op();
    receive_op.kind = impl::io_op_kind::receive;
    receive_op.socket = s.handle;
    receive_op.buffer = reinterpret_cast<byte*>(inbox);
    receive_op.buffer_size = isize(sizeof(inbox));
    receive_op.deadline_ns = clk.now_ns() + 30ll * 1000 * 1000 * 1000; // 30 s away, and nobody will ever send
    r.submit(&receive_op);

    // Nothing has arrived and the clock has not moved, so the operation stays pending.
    CHECK(r.wait(0) == 0);
    CHECK(!receive_op.completed);

    clk.advance_ms(30'001);
    CHECK(r.wait(0) == 1);
    CHECK(receive_op.completed);
    CHECK(receive_op.code() == error_code::timed_out);
}

TEST("cnet - an idle reactor reports no progress, which is what the pump registry requires")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto clk = manual_clock(0);
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    // A registration that always claims progress turns every blocking wait in the process into a busy loop, since a
    // driver treats "no progress anywhere" as its cue to sleep -- the contract in clean-core's thread_pump.hh.
    CHECK(r.pending_count() == 0);
    CHECK(r.wait(0) == 0);
    CHECK(r.wait(0) == 0);
}

TEST("cnet - a cancelled operation completes as cancelled")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto clk = manual_clock(0);
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    auto created = impl::create_udp_socket(ip_family::v4);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());
    CHECK(impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());

    char inbox[8] = {};
    auto receive_op = capture_op();
    receive_op.kind = impl::io_op_kind::receive;
    receive_op.socket = s.handle;
    receive_op.buffer = reinterpret_cast<byte*>(inbox);
    receive_op.buffer_size = isize(sizeof(inbox));
    r.submit(&receive_op);

    CHECK(r.pending_count() == 1);
    r.cancel(&receive_op);

    CHECK(r.wait(0) == 1);
    CHECK(receive_op.completed);
    CHECK(receive_op.code() == error_code::cancelled);
    CHECK(r.pending_count() == 0);

    // Cancelling something already gone is harmless, which is what makes a cancel racing a completion safe.
    r.cancel(&receive_op);
    CHECK(r.wait(0) == 0);
}

TEST("cnet - wake ends a wait that would otherwise sit there")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto& clk = system_clock();
    auto reactor = impl::reactor::try_create(clk);
    CHECK(reactor.has_value());
    auto& r = *reactor.value();

    r.wake();

    auto const before = clk.now_ns();
    CHECK(r.wait(30000) == 0); // nothing pending, so nothing completes -- the point is that it returns at all
    auto const elapsed_ms = (clk.now_ns() - before) / (1000 * 1000);

    // Thirty seconds of slack against an expected zero, so this pins the mechanism without pinning the schedule.
    CHECK(elapsed_ms < 5000);
}
