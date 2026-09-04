#include <clean-net/impl/native_socket.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// These bind and listen on loopback and never connect outward, so they are hermetic in the sense the suite requires:
// no name is resolved, no packet leaves the machine, and nothing outside this process has to be running.

namespace
{
/// Close on every exit path, including a failed CHECK that returns early.
struct socket_guard
{
    impl::native_socket handle = impl::k_invalid_socket;

    socket_guard() = default;
    explicit socket_guard(impl::native_socket s) : handle(s) {}
    socket_guard(socket_guard const&) = delete;
    socket_guard& operator=(socket_guard const&) = delete;
    ~socket_guard() { impl::close_socket(handle); }
};
} // namespace

TEST("cnet - a TCP socket binds to loopback and reports the port it got")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto created = impl::create_tcp_socket(ip_family::v4);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());

    auto const bound = impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true);
    CHECK(bound.has_value());

    auto const local = impl::local_endpoint(s.handle);
    CHECK(local.has_value());
    CHECK(local.value().address.is_loopback());

    // Port 0 asked the OS to choose, so the answer is what makes a test server addressable.
    CHECK(local.value().port != 0);

    CHECK(impl::listen_socket(s.handle, 8).has_value());
}

TEST("cnet - IPv6 binds too, and v6-only is set rather than inherited")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto created = impl::create_tcp_socket(ip_family::v6);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());

    // The OS default differs by platform and by sysctl, so a caller that does not say gets a different socket on two machines.
    // Setting it either way is the point of the test.
    CHECK(impl::set_v6_only(s.handle, true).has_value());

    auto const bound = impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v6), 0), true);
    CHECK(bound.has_value());

    auto const local = impl::local_endpoint(s.handle);
    CHECK(local.has_value());
    CHECK(local.value().address.family() == ip_family::v6);
    CHECK(local.value().address.is_loopback());
}

TEST("cnet - binding a port twice reports address_in_use rather than a generic failure")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto first_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(first_created.has_value());
    auto const first = socket_guard(first_created.value());

    // No SO_REUSEADDR on the first: with it, POSIX would let the second bind succeed and the test would prove nothing.
    CHECK(impl::bind_socket(first.handle, endpoint(ip_address::loopback(ip_family::v4), 0), false).has_value());
    CHECK(impl::listen_socket(first.handle, 8).has_value());

    auto const taken = impl::local_endpoint(first.handle);
    CHECK(taken.has_value());

    auto second_created = impl::create_tcp_socket(ip_family::v4);
    CHECK(second_created.has_value());
    auto const second = socket_guard(second_created.value());

    auto const clash = impl::bind_socket(second.handle, taken.value(), false);
    CHECK(clash.has_error());
    CHECK(clash.error().code == error_code::address_in_use);

    // The platform's own number is kept, which is what makes an unmapped failure diagnosable.
    CHECK(clash.error().native_code != 0);
}

TEST("cnet - socket options apply to a fresh socket")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto created = impl::create_tcp_socket(ip_family::v4);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());

    // A request/response protocol over a Nagled socket pays 40 ms for a small write following a small write.
    CHECK(impl::set_tcp_no_delay(s.handle, true).has_value());
    CHECK(impl::set_tcp_no_delay(s.handle, false).has_value());
}

TEST("cnet - a UDP socket binds without listening")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto created = impl::create_udp_socket(ip_family::v4);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());

    CHECK(impl::bind_socket(s.handle, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_value());
    auto const local = impl::local_endpoint(s.handle);
    CHECK(local.has_value());
    CHECK(local.value().port != 0);
}

TEST("cnet - a socket needs an address family, and an unbound one has no peer")
{
    if (!impl::sockets_are_supported())
        SKIP("this platform has no sockets");

    auto const no_family = impl::create_tcp_socket(ip_family::none);
    CHECK(no_family.has_error());
    CHECK(no_family.error().code == error_code::invalid_argument);
    CHECK(no_family.error().native_code == 0); // no platform call was made, so there is no number to report

    auto created = impl::create_tcp_socket(ip_family::v4);
    CHECK(created.has_value());
    auto const s = socket_guard(created.value());

    // Not connected: this fails rather than reporting a zero endpoint that reads like a real one.
    CHECK(impl::remote_endpoint(s.handle).has_error());
}

TEST("cnet - the socket availability contract holds in both build modes")
{
    auto created = impl::create_tcp_socket(ip_family::v4);

    if (impl::sockets_are_supported())
    {
        CHECK(created.has_value());
        impl::close_socket(created.value());
        return;
    }

    // On wasm this is the whole story: not a socket that needs a flag, none at all.
    CHECK(created.has_error());
    CHECK(created.error().code == error_code::unsupported);
    CHECK(impl::bind_socket(impl::k_invalid_socket, endpoint(ip_address::loopback(ip_family::v4), 0), true).has_error());
}
