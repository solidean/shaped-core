#include "native_socket.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/transport/datagram.hh>

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
#include <mstcpip.h>
#include <mswsock.h>
// clang-format on
#else
#if defined(__APPLE__)
// Apple declares the RFC 3542 IPv6 socket options, IPV6_DONTFRAG among them, only when asked to, before the headers.
#define __APPLE_USE_RFC_3542 1
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
    case WSAEMSGSIZE:
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
    case EMSGSIZE:
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

cc::result<cc::unit, error> connect_socket(native_socket s, endpoint const& where, bool fail_fast_on_refused)
{
    sockaddr_storage addr = {};
    socket_length length = 0;
    if (!to_sockaddr(where, addr, length))
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("connect: the endpoint has no address")});

#if defined(_WIN32)
    // Windows answers a refused connection by retransmitting the SYN, so a closed port takes about two seconds to refuse even on loopback.
    // Those retries are also what rides out a full listen backlog, which Windows refuses rather than drops.
    // Turning them off therefore fails a connect burst that would have succeeded once the server accepted, which is why it is opt-in.
    // Remote peers keep the default regardless: there a retransmission is what survives a dropped packet.
    // Best effort, since an older stack without the ioctl still connects, just slowly.
    if (fail_fast_on_refused && where.address.is_loopback())
    {
        auto rto = TCP_INITIAL_RTO_PARAMETERS{.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT,
                                              .MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
        auto returned = DWORD(0);
        (void)::WSAIoctl(raw_of(s), SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0, &returned, nullptr, nullptr);
    }
#else
    (void)fail_fast_on_refused;
#endif

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

// ---- datagrams -----------------------------------------------------------------------------------------

namespace
{
/// Whether the platform is saying "nothing more right now" rather than "no".
[[nodiscard]] bool is_would_block_native(i32 native)
{
#if defined(_WIN32)
    return native == WSAEWOULDBLOCK;
#else
    return native == EWOULDBLOCK || native == EAGAIN || native == EINTR;
#endif
}

/// Whether a send stopped because the OS has no room for the datagram right now, which is not an error.
/// macOS and the BSDs say so with ENOBUFS rather than EAGAIN when the interface queue is full, and Linux can too.
[[nodiscard]] bool is_send_buffer_full(i32 native)
{
#if defined(_WIN32)
    return is_would_block_native(native) || native == WSAENOBUFS;
#else
    return is_would_block_native(native) || native == ENOBUFS;
#endif
}

/// A receive failure that is really about an earlier send to a peer that refused, which for datagrams is loss.
[[nodiscard]] bool is_echo_of_a_refusal(i32 native)
{
#if defined(_WIN32)
    return native == WSAECONNRESET || native == WSAENETRESET;
#else
    return native == ECONNREFUSED;
#endif
}

[[nodiscard]] error no_address(cc::string_view what)
{
    return {.code = error_code::invalid_argument,
            .native_code = 0,
            .message = cc::format("{}: the endpoint has no address", what)};
}

[[nodiscard]] cc::result<cc::unit, error> set_int_option(native_socket s, i32 level, i32 name, i32 value, cc::string_view what)
{
    int const v = value;
    if (::setsockopt(raw_of(s), level, name, reinterpret_cast<char const*>(&v), sizeof(v)) != 0)
        return cc::error(error_from_native(last_socket_error(), what));
    return cc::unit{};
}

[[nodiscard]] cc::result<cc::unit, error> set_dont_fragment(native_socket s, ip_family family)
{
#if defined(_WIN32)
    if (family == ip_family::v6)
        return set_int_option(s, IPPROTO_IPV6, IPV6_DONTFRAG, 1, "setting IPV6_DONTFRAG");
    return set_int_option(s, IPPROTO_IP, IP_DONTFRAGMENT, 1, "setting IP_DONTFRAGMENT");
#elif defined(IP_MTU_DISCOVER)
    // Linux spells it as a discovery mode: DO means never fragment locally, and fail a send larger than the path.
    if (family == ip_family::v6)
        return set_int_option(s, IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_DO, "setting IPV6_MTU_DISCOVER");
    return set_int_option(s, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO, "setting IP_MTU_DISCOVER");
#elif defined(IP_DONTFRAG)
    if (family == ip_family::v6)
    {
#if defined(IPV6_DONTFRAG)
        return set_int_option(s, IPPROTO_IPV6, IPV6_DONTFRAG, 1, "setting IPV6_DONTFRAG");
#else
        return cc::error(unsupported_here("refusing to fragment an IPv6 datagram"));
#endif
    }
    return set_int_option(s, IPPROTO_IP, IP_DONTFRAG, 1, "setting IP_DONTFRAG");
#else
    (void)s;
    (void)family;
    return cc::error(unsupported_here("refusing to fragment a datagram"));
#endif
}

#if defined(__linux__)
/// The most records one `recvmmsg` or `sendmmsg` call carries; a longer batch is several calls.
constexpr isize k_mmsg_batch = 64;
#endif
} // namespace

cc::result<cc::unit, error> apply_datagram_options(native_socket s, ip_family family, datagram_options const& options)
{
#if defined(_WIN32)
    // Without this, a send that draws an ICMP "port unreachable" makes the NEXT receive fail with WSAECONNRESET, so a
    // peer that went away looks like a broken socket; every other platform reports nothing on an unconnected socket.
    // SIO_UDP_NETRESET is the same for an ICMP "TTL expired".
    // Best effort: the receive path skips both errors anyway.
    BOOL off = FALSE;
    auto returned = DWORD(0);
    (void)::WSAIoctl(raw_of(s), SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
    (void)::WSAIoctl(raw_of(s), SIO_UDP_NETRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
#endif

    if (family == ip_family::v6)
        if (auto r = set_v6_only(s, options.v6_only); r.has_error())
            return r;

    if (options.receive_buffer_bytes > 0)
        if (auto r = set_int_option(s, SOL_SOCKET, SO_RCVBUF, options.receive_buffer_bytes, "setting SO_RCVBUF");
            r.has_error())
            return r;

    if (options.send_buffer_bytes > 0)
        if (auto r = set_int_option(s, SOL_SOCKET, SO_SNDBUF, options.send_buffer_bytes, "setting SO_SNDBUF");
            r.has_error())
            return r;

    if (options.dont_fragment)
        if (auto r = set_dont_fragment(s, family); r.has_error())
            return r;

    // Best effort: Windows ignores it without a QoS policy, and the network may rewrite it anyway.
    if (options.traffic_class != 0)
    {
        if (family == ip_family::v6)
            (void)set_int_option(s, IPPROTO_IPV6, IPV6_TCLASS, options.traffic_class, "setting IPV6_TCLASS");
        else
            (void)set_int_option(s, IPPROTO_IP, IP_TOS, options.traffic_class, "setting IP_TOS");
    }

    return cc::unit{};
}

#if defined(__linux__)

cc::result<isize, error> receive_datagrams(native_socket s, cc::span<datagram> records, clock& stamp)
{
    mmsghdr headers[k_mmsg_batch];
    iovec vectors[k_mmsg_batch];
    sockaddr_storage addresses[k_mmsg_batch];

    auto filled = isize(0);
    while (filled < records.size())
    {
        auto const chunk = cc::min(records.size() - filled, k_mmsg_batch);
        for (isize i = 0; i < chunk; ++i)
        {
            auto& r = records[filled + i];
            vectors[i] = {.iov_base = r.buffer.data(), .iov_len = size_t(r.buffer.size())};
            headers[i] = {};
            headers[i].msg_hdr.msg_name = &addresses[i];
            headers[i].msg_hdr.msg_namelen = socklen_t(sizeof(addresses[i]));
            headers[i].msg_hdr.msg_iov = &vectors[i];
            headers[i].msg_hdr.msg_iovlen = 1;
        }

        auto const n = ::recvmmsg(raw_of(s), headers, unsigned(chunk), MSG_DONTWAIT, nullptr);
        if (n < 0)
        {
            auto const native = last_socket_error();
            if (is_would_block_native(native))
                return filled;
            if (is_echo_of_a_refusal(native))
                continue;
            if (filled > 0)
                return filled;
            return cc::error(error_from_native(native, "receiving datagrams"));
        }

        auto const now = stamp.now_ns();
        for (isize i = 0; i < isize(n); ++i)
        {
            auto& r = records[filled + i];
            auto const peer = from_sockaddr(addresses[i]);
            r.size = isize(headers[i].msg_len);
            r.peer = peer.has_value() ? peer.value() : endpoint();
            r.received_at_ns = now;
            r.truncated = (headers[i].msg_hdr.msg_flags & MSG_TRUNC) != 0;
        }
        filled += isize(n);

        // A short batch means the socket ran dry, and asking again would only be told so.
        if (isize(n) < chunk)
            return filled;
    }
    return filled;
}

cc::result<isize, error> send_datagrams(native_socket s, cc::span<outgoing_datagram const> packets)
{
    mmsghdr headers[k_mmsg_batch];
    iovec vectors[k_mmsg_batch];
    sockaddr_storage addresses[k_mmsg_batch];

    auto sent = isize(0);
    while (sent < packets.size())
    {
        // A packet with no address ends the chunk before it, so the ones ahead of it still go out.
        auto chunk = isize(0);
        auto const most = cc::min(packets.size() - sent, k_mmsg_batch);
        for (; chunk < most; ++chunk)
        {
            auto const& p = packets[sent + chunk];
            auto length = socklen_t(0);
            if (!to_sockaddr(p.peer, addresses[chunk], length))
                break;
            vectors[chunk] = {.iov_base = const_cast<byte*>(p.bytes.data()), .iov_len = size_t(p.bytes.size())};
            headers[chunk] = {};
            headers[chunk].msg_hdr.msg_name = &addresses[chunk];
            headers[chunk].msg_hdr.msg_namelen = length;
            headers[chunk].msg_hdr.msg_iov = &vectors[chunk];
            headers[chunk].msg_hdr.msg_iovlen = 1;
        }

        if (chunk == 0)
        {
            if (sent > 0)
                return sent;
            return cc::error(no_address("sending a datagram"));
        }

        auto const n = ::sendmmsg(raw_of(s), headers, unsigned(chunk), MSG_DONTWAIT);
        if (n < 0)
        {
            auto const native = last_socket_error();
            if (is_send_buffer_full(native) || sent > 0)
                return sent;
            return cc::error(error_from_native(native, cc::format("sending a datagram to {}", packets[sent].peer)));
        }

        sent += isize(n);

        // Short means the kernel stopped at a packet it would not take, or the chunk ended at one without an address;
        // either way the next call, starting there, says why.
        if (isize(n) < most)
            return sent;
    }
    return sent;
}

#else // one datagram per syscall

cc::result<isize, error> receive_datagrams(native_socket s, cc::span<datagram> records, clock& stamp)
{
    auto filled = isize(0);
    while (filled < records.size())
    {
        auto& r = records[filled];
        sockaddr_storage addr = {};
        auto truncated = false;

#if defined(_WIN32)
        auto length = socket_length(sizeof(addr));
        auto n = ::recvfrom(raw_of(s), reinterpret_cast<char*>(r.buffer.data()), int(r.buffer.size()), 0,
                            reinterpret_cast<sockaddr*>(&addr), &length);
        if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEMSGSIZE)
        {
            // Windows fails a datagram that did not fit, after filling the buffer with as much of it as did.
            n = int(r.buffer.size());
            truncated = true;
        }
#else
        // recvmsg rather than recvfrom: only the message flags say a datagram was cut, and only recvmsg returns them.
        iovec vec = {.iov_base = r.buffer.data(), .iov_len = size_t(r.buffer.size())};
        msghdr header = {};
        header.msg_name = &addr;
        header.msg_namelen = socket_length(sizeof(addr));
        header.msg_iov = &vec;
        header.msg_iovlen = 1;
        auto const n = ::recvmsg(raw_of(s), &header, 0);
        if (n >= 0)
            truncated = (header.msg_flags & MSG_TRUNC) != 0;
#endif

        if (n < 0)
        {
            auto const native = last_socket_error();
            if (is_would_block_native(native))
                return filled;
            if (is_echo_of_a_refusal(native))
                continue;
            if (filled > 0)
                return filled;
            return cc::error(error_from_native(native, "receiving a datagram"));
        }

        auto const peer = from_sockaddr(addr);
        r.size = isize(n);
        r.peer = peer.has_value() ? peer.value() : endpoint();
        r.received_at_ns = stamp.now_ns();
        r.truncated = truncated;
        ++filled;
    }
    return filled;
}

cc::result<isize, error> send_datagrams(native_socket s, cc::span<outgoing_datagram const> packets)
{
    for (isize i = 0; i < packets.size(); ++i)
    {
        auto const& p = packets[i];
        sockaddr_storage addr = {};
        auto length = socket_length(0);
        if (!to_sockaddr(p.peer, addr, length))
        {
            if (i > 0)
                return i;
            return cc::error(no_address("sending a datagram"));
        }

        auto const n = ::sendto(raw_of(s), reinterpret_cast<char const*>(p.bytes.data()), int(p.bytes.size()), 0,
                                reinterpret_cast<sockaddr const*>(&addr), length);
        if (n < 0)
        {
            auto const native = last_socket_error();
            if (is_send_buffer_full(native) || i > 0)
                return i;
            return cc::error(error_from_native(native, cc::format("sending a datagram to {}", p.peer)));
        }
    }
    return packets.size();
}

#endif
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
cc::result<cc::unit, error> connect_socket(native_socket, endpoint const&, bool)
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
cc::result<cc::unit, error> apply_datagram_options(native_socket, ip_family, datagram_options const&)
{
    return cc::error(no_sockets("setting datagram options"));
}
cc::result<isize, error> receive_datagrams(native_socket, cc::span<datagram>, clock&)
{
    return cc::error(no_sockets("receiving datagrams"));
}
cc::result<isize, error> send_datagrams(native_socket, cc::span<outgoing_datagram const>)
{
    return cc::error(no_sockets("sending datagrams"));
}
} // namespace cnet::impl

#endif // CNET_HAS_SOCKETS
