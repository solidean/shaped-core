#include "native_socket.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

// CNET_HAS_SOCKETS is a PRIVATE define set by clean-net's CMakeLists: 1 where the platform has BSD sockets, 0 on
// wasm, which has none of any kind.
// At 0 this file compiles a complete stub whose entry points report the platform unsupported at runtime.
// The switch never leaves this file.
#ifndef CNET_HAS_SOCKETS
#define CNET_HAS_SOCKETS 0
#endif

#if CNET_HAS_SOCKETS

#if defined(_WIN32)
// clang-format off
// winsock2 must precede ws2tcpip, whose declarations are written in terms of it.
// Sorted order puts ws2tcpip first and does not compile.
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cnet::impl
{
namespace
{
#if defined(_WIN32)
using socket_length = int;
using raw_socket = SOCKET;
constexpr raw_socket k_raw_invalid = INVALID_SOCKET;
#else
using socket_length = socklen_t;
using raw_socket = int;
constexpr raw_socket k_raw_invalid = -1;
#endif

[[nodiscard]] raw_socket raw_of(native_socket s)
{
    return raw_socket(s);
}

[[nodiscard]] i32 af_of(ip_family family)
{
    return family == ip_family::v6 ? AF_INET6 : AF_INET;
}

/// Fill a `sockaddr_storage` from an endpoint, and report how many bytes of it are meaningful.
[[nodiscard]] bool to_sockaddr(endpoint const& e, sockaddr_storage& out, socket_length& out_length)
{
    out = {};
    auto const octets = e.address.octets();

    if (e.address.family() == ip_family::v4)
    {
        auto& v4 = reinterpret_cast<sockaddr_in&>(out);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(u16(e.port));
        auto* const dst = reinterpret_cast<u8*>(&v4.sin_addr);
        for (isize i = 0; i < 4; ++i)
            dst[i] = octets[i];
        out_length = socket_length(sizeof(sockaddr_in));
        return true;
    }

    if (e.address.family() == ip_family::v6)
    {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(out);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(u16(e.port));
        v6.sin6_scope_id = e.address.scope_id();
        auto* const dst = reinterpret_cast<u8*>(&v6.sin6_addr);
        for (isize i = 0; i < 16; ++i)
            dst[i] = octets[i];
        out_length = socket_length(sizeof(sockaddr_in6));
        return true;
    }

    return false;
}

[[nodiscard]] cc::optional<endpoint> from_sockaddr(sockaddr_storage const& addr)
{
    if (addr.ss_family == AF_INET)
    {
        auto const& v4 = reinterpret_cast<sockaddr_in const&>(addr);
        auto const* const src = reinterpret_cast<u8 const*>(&v4.sin_addr);
        return endpoint(ip_address::from_v4(cc::span<u8 const>(src, 4)), i32(ntohs(v4.sin_port)));
    }

    if (addr.ss_family == AF_INET6)
    {
        auto const& v6 = reinterpret_cast<sockaddr_in6 const&>(addr);
        auto const* const src = reinterpret_cast<u8 const*>(&v6.sin6_addr);
        return endpoint(ip_address::from_v6(cc::span<u8 const>(src, 16), u32(v6.sin6_scope_id)),
                        i32(ntohs(v6.sin6_port)));
    }

    return {};
}

[[nodiscard]] cc::result<cc::unit, error> set_non_blocking_raw(raw_socket s)
{
#if defined(_WIN32)
    u_long one = 1;
    if (::ioctlsocket(s, FIONBIO, &one) != 0)
        return cc::error(error_from_native(last_socket_error(), "setting a socket non-blocking"));
#else
    // fcntl rather than SOCK_NONBLOCK: the flag is a Linux extension, and Darwin does not have it.
    auto const flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0 || ::fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        return cc::error(error_from_native(last_socket_error(), "setting a socket non-blocking"));
#endif
    return cc::unit{};
}

[[nodiscard]] cc::result<native_socket, error> create_socket(ip_family family, i32 type, i32 protocol, cc::string_view what)
{
    if (family == ip_family::none)
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::format("{}: no address family", what)});

    ensure_socket_platform();

#if defined(_WIN32)
    // WSA_FLAG_OVERLAPPED is what makes the socket usable with an I/O completion port, and it is not the default for
    // a socket created with ::socket().
    auto const s = ::WSASocketW(af_of(family), type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
#else
    auto const s = ::socket(af_of(family), type, protocol);
#endif
    if (s == k_raw_invalid)
        return cc::error(error_from_native(last_socket_error(), what));

    auto non_blocking = set_non_blocking_raw(s);
    if (non_blocking.has_error())
    {
        close_socket(native_socket(s));
        return cc::error(cc::move(non_blocking).error());
    }

    return native_socket(s);
}
} // namespace

bool sockets_are_supported()
{
    return true;
}

cc::result<cc::unit, error> set_socket_non_blocking(native_socket s)
{
    return set_non_blocking_raw(raw_of(s));
}

void ensure_socket_platform()
{
#if defined(_WIN32)
    // Never torn down: WSACleanup racing a socket still in flight is a worse outcome than a leaked initialization,
    // and the process is going away regardless.
    static auto const started = []
    {
        WSADATA data = {};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

i32 last_socket_error()
{
#if defined(_WIN32)
    return i32(::WSAGetLastError());
#else
    return i32(errno);
#endif
}

error error_from_native(i32 native, cc::string_view what)
{
    auto code = error_code::unknown;
    switch (native)
    {
#if defined(_WIN32)
    case WSAECONNREFUSED:
        code = error_code::connection_refused;
        break;
    case WSAECONNRESET:
    case WSAECONNABORTED:
        code = error_code::connection_reset;
        break;
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH:
        code = error_code::host_unreachable;
        break;
    case WSAEADDRINUSE:
        code = error_code::address_in_use;
        break;
    case WSAEACCES:
        code = error_code::permission_denied;
        break;
    case WSAETIMEDOUT:
        code = error_code::timed_out;
        break;
    case WSAEINVAL:
    case WSAEAFNOSUPPORT:
        code = error_code::invalid_argument;
        break;
#else
    case ECONNREFUSED:
        code = error_code::connection_refused;
        break;
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:
        code = error_code::connection_reset;
        break;
    case EHOSTUNREACH:
    case ENETUNREACH:
        code = error_code::host_unreachable;
        break;
    case EADDRINUSE:
        code = error_code::address_in_use;
        break;
    case EACCES:
    case EPERM:
        code = error_code::permission_denied;
        break;
    case ETIMEDOUT:
        code = error_code::timed_out;
        break;
    case EINVAL:
    case EAFNOSUPPORT:
        code = error_code::invalid_argument;
        break;
#endif
    default:
        break;
    }

    return {.code = code,
            .native_code = native,
            .message = cc::format("{} failed ({}, code {})", what, to_string(code), native)};
}

cc::result<native_socket, error> create_tcp_socket(ip_family family)
{
    return create_socket(family, SOCK_STREAM, IPPROTO_TCP, "creating a TCP socket");
}

cc::result<native_socket, error> create_udp_socket(ip_family family)
{
    return create_socket(family, SOCK_DGRAM, IPPROTO_UDP, "creating a UDP socket");
}

void close_socket(native_socket s)
{
    if (s == k_invalid_socket)
        return;
#if defined(_WIN32)
    ::closesocket(raw_of(s));
#else
    ::close(raw_of(s));
#endif
}

cc::result<cc::unit, error> shutdown_socket_send(native_socket s)
{
    // SD_SEND and SHUT_WR are the same constant under two names, and neither header defines the other's.
#if defined(_WIN32)
    auto const how = SD_SEND;
#else
    auto const how = SHUT_WR;
#endif
    if (::shutdown(raw_of(s), how) != 0)
        return cc::error(error_from_native(last_socket_error(), "shutting down the sending half"));
    return cc::unit{};
}

cc::result<cc::unit, error> bind_socket(native_socket s, endpoint const& where, bool reuse_address)
{
#if !defined(_WIN32)
    if (reuse_address)
    {
        int const one = 1;
        if (::setsockopt(raw_of(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&one), sizeof(one)) != 0)
            return cc::error(error_from_native(last_socket_error(), "setting SO_REUSEADDR"));
    }
#else
    // Deliberately not set on Windows: SO_REUSEADDR there permits STEALING a port another socket already holds,
    // rather than reusing one left in TIME_WAIT, which is not what any caller of this means.
    (void)reuse_address;
#endif

    sockaddr_storage addr = {};
    socket_length length = 0;
    if (!to_sockaddr(where, addr, length))
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("bind: the endpoint has no address")});

    if (::bind(raw_of(s), reinterpret_cast<sockaddr const*>(&addr), length) != 0)
        return cc::error(error_from_native(last_socket_error(), cc::format("binding to {}", where)));
    return cc::unit{};
}

cc::result<cc::unit, error> listen_socket(native_socket s, i32 backlog)
{
    if (::listen(raw_of(s), backlog) != 0)
        return cc::error(error_from_native(last_socket_error(), "listening"));
    return cc::unit{};
}

cc::result<cc::unit, error> connect_socket(native_socket s, endpoint const& where)
{
    sockaddr_storage addr = {};
    socket_length length = 0;
    if (!to_sockaddr(where, addr, length))
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("connect: the endpoint has no address")});

    if (::connect(raw_of(s), reinterpret_cast<sockaddr const*>(&addr), length) == 0)
        return cc::unit{};

    auto const native = last_socket_error();
#if defined(_WIN32)
    auto const in_progress = native == WSAEWOULDBLOCK || native == WSAEINPROGRESS || native == WSAEALREADY;
#else
    auto const in_progress
        = native == EINPROGRESS || native == EWOULDBLOCK || native == EAGAIN || native == EALREADY || native == EINTR;
#endif
    if (in_progress)
        return cc::unit{};

    return cc::error(error_from_native(native, cc::format("connecting to {}", where)));
}

cc::result<native_socket, error> accept_socket(native_socket listener)
{
    sockaddr_storage addr = {};
    socket_length length = socket_length(sizeof(addr));
    auto const accepted = ::accept(raw_of(listener), reinterpret_cast<sockaddr*>(&addr), &length);
    if (accepted == k_raw_invalid)
        return cc::error(error_from_native(last_socket_error(), "accepting"));

    // Linux does not let an accepted socket inherit O_NONBLOCK from its listener, and BSD does.
    auto non_blocking = set_non_blocking_raw(accepted);
    if (non_blocking.has_error())
    {
        close_socket(native_socket(accepted));
        return cc::error(cc::move(non_blocking).error());
    }
    return native_socket(accepted);
}

void drain_datagrams(native_socket s)
{
    char scratch[64];
    while (::recv(raw_of(s), scratch, int(sizeof(scratch)), 0) > 0)
    {
    }
}

cc::result<endpoint, error> local_endpoint(native_socket s)
{
    sockaddr_storage addr = {};
    socket_length length = socket_length(sizeof(addr));
    if (::getsockname(raw_of(s), reinterpret_cast<sockaddr*>(&addr), &length) != 0)
        return cc::error(error_from_native(last_socket_error(), "reading the local endpoint"));

    auto const e = from_sockaddr(addr);
    if (!e.has_value())
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the local endpoint is of an unknown family")});
    return e.value();
}

cc::result<endpoint, error> remote_endpoint(native_socket s)
{
    sockaddr_storage addr = {};
    socket_length length = socket_length(sizeof(addr));
    if (::getpeername(raw_of(s), reinterpret_cast<sockaddr*>(&addr), &length) != 0)
        return cc::error(error_from_native(last_socket_error(), "reading the remote endpoint"));

    auto const e = from_sockaddr(addr);
    if (!e.has_value())
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the remote endpoint is of an unknown family")});
    return e.value();
}

cc::result<cc::unit, error> set_v6_only(native_socket s, bool v6_only)
{
    int const value = v6_only ? 1 : 0;
    if (::setsockopt(raw_of(s), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char const*>(&value), sizeof(value)) != 0)
        return cc::error(error_from_native(last_socket_error(), "setting IPV6_V6ONLY"));
    return cc::unit{};
}

cc::result<cc::unit, error> set_tcp_no_delay(native_socket s, bool no_delay)
{
    int const value = no_delay ? 1 : 0;
    if (::setsockopt(raw_of(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&value), sizeof(value)) != 0)
        return cc::error(error_from_native(last_socket_error(), "setting TCP_NODELAY"));
    return cc::unit{};
}
} // namespace cnet::impl

#else // CNET_HAS_SOCKETS -- the platform has no sockets of any kind

namespace cnet::impl
{
namespace
{
error no_sockets(cc::string_view what)
{
    return unsupported_here(cc::format("{}: a socket", what));
}
} // namespace

bool sockets_are_supported()
{
    return false;
}

void ensure_socket_platform()
{
}

i32 last_socket_error()
{
    return 0;
}

error error_from_native(i32 native, cc::string_view what)
{
    return {.code = error_code::unsupported,
            .native_code = native,
            .message = cc::format("{}: this platform has no sockets", what)};
}

cc::result<native_socket, error> create_tcp_socket(ip_family)
{
    return cc::error(no_sockets("creating a TCP socket"));
}
cc::result<native_socket, error> create_udp_socket(ip_family)
{
    return cc::error(no_sockets("creating a UDP socket"));
}

void close_socket(native_socket)
{
}
cc::result<cc::unit, error> shutdown_socket_send(native_socket)
{
    return cc::error(no_sockets("shutting down the sending half"));
}

cc::result<cc::unit, error> bind_socket(native_socket, endpoint const&, bool)
{
    return cc::error(no_sockets("binding"));
}
cc::result<cc::unit, error> listen_socket(native_socket, i32)
{
    return cc::error(no_sockets("listening"));
}
cc::result<cc::unit, error> connect_socket(native_socket, endpoint const&)
{
    return cc::error(no_sockets("connecting"));
}
cc::result<native_socket, error> accept_socket(native_socket)
{
    return cc::error(no_sockets("accepting"));
}
void drain_datagrams(native_socket)
{
}
cc::result<endpoint, error> local_endpoint(native_socket)
{
    return cc::error(no_sockets("reading the local endpoint"));
}
cc::result<endpoint, error> remote_endpoint(native_socket)
{
    return cc::error(no_sockets("reading the remote endpoint"));
}
cc::result<cc::unit, error> set_v6_only(native_socket, bool)
{
    return cc::error(no_sockets("setting IPV6_V6ONLY"));
}
cc::result<cc::unit, error> set_tcp_no_delay(native_socket, bool)
{
    return cc::error(no_sockets("setting TCP_NODELAY"));
}
} // namespace cnet::impl

#endif // CNET_HAS_SOCKETS
