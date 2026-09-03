#pragma once

#include <clean-core/error/result.hh>
#include <clean-net/address/endpoint.hh>
#include <clean-net/common/error.hh>

/// The thin layer over the platform's sockets, and the only place an OS socket type is named.
///
/// Internal: nothing here appears in a public clean-net header, so `SOCKET`, `sockaddr` and `WSAGetLastError` stay on
/// this side of the wall.
///
/// It is deliberately synchronous and non-blocking rather than asynchronous: the reactor above it owns the waiting,
/// and mixing the two would give two things the right to decide when a call returns.

namespace cnet::impl
{
/// A platform socket, or `k_invalid_socket`.
///
/// An integer on POSIX and a pointer-sized `SOCKET` on Windows, so it is carried as the wider of the two rather than
/// as an `int` that would truncate on Windows.
using native_socket = u64;

/// The value a socket variable holds when it owns nothing.
///
/// Not -1: Windows spells its invalid socket as `~0ull` and POSIX as -1, and those are the same bit pattern here only
/// because this type is unsigned.
inline constexpr native_socket k_invalid_socket = ~native_socket(0);

/// Whether this build has sockets at all.
///
/// False on wasm, where the browser offers no socket of any kind -- not a socket that needs a flag, none at all.
/// Every function below returns `error_code::unsupported` there rather than disappearing.
[[nodiscard]] bool sockets_are_supported();

/// One-time platform start-up, needed by Winsock and by nothing else.
///
/// Idempotent and thread-safe, and never torn down: the process is going away anyway, and a teardown racing a socket
/// still in flight is a worse outcome than a leaked initialization.
void ensure_socket_platform();

/// Translate the platform's own error number into ours.
///
/// `native` is `WSAGetLastError()` on Windows and `errno` on POSIX; pass what the failing call reported rather than
/// reading it again, since the intervening calls may have replaced it.
[[nodiscard]] error error_from_native(i32 native, cc::string_view what);

/// The platform's error number for the call that just failed on this thread.
[[nodiscard]] i32 last_socket_error();

/// Create a non-blocking TCP socket of `family`.
///
/// Non-blocking from birth rather than switched afterwards: a socket that is briefly blocking is a socket that can
/// briefly stall the reactor thread, and the window is invisible in testing.
[[nodiscard]] cc::result<native_socket, error> create_tcp_socket(ip_family family);

/// Create a non-blocking UDP socket of `family`.
[[nodiscard]] cc::result<native_socket, error> create_udp_socket(ip_family family);

/// Put an existing socket into non-blocking mode.
///
/// Needed for a socket that arrived rather than one we created: Linux does NOT let an accepted socket inherit
/// O_NONBLOCK from its listener, while BSD does, so an accepted socket that nobody set is blocking on exactly one
/// of our platforms.
[[nodiscard]] cc::result<cc::unit, error> set_socket_non_blocking(native_socket s);

void close_socket(native_socket s);

/// Bind to `where`.
///
/// `reuse_address` sets SO_REUSEADDR on POSIX and nothing on Windows, where that option means something else
/// entirely -- it permits *stealing* a bound port rather than reusing one in TIME_WAIT, which is not what any caller
/// of this wants.
[[nodiscard]] cc::result<cc::unit, error> bind_socket(native_socket s, endpoint const& where, bool reuse_address);

[[nodiscard]] cc::result<cc::unit, error> listen_socket(native_socket s, i32 backlog);

/// Start connecting to `where`.
///
/// Succeeds both when the connection completed at once -- which loopback often does -- and when it is merely under
/// way: on a non-blocking socket "in progress" is the normal answer, and the reactor learns the outcome from
/// writability plus SO_ERROR rather than from this call.
/// Only an outright refusal by the local stack is reported here.
[[nodiscard]] cc::result<cc::unit, error> connect_socket(native_socket s, endpoint const& where);

/// Take one pending connection off a listening socket; the result is non-blocking.
///
/// When nothing is pending this fails with the platform's would-block number in `native_code` rather than blocking,
/// which is how the reactor tells "not yet" from "no".
[[nodiscard]] cc::result<native_socket, error> accept_socket(native_socket listener);

/// Read and discard whatever is waiting on a datagram socket.
/// Used for the reactor's self-wake channel, where the bytes carry no meaning at all.
void drain_datagrams(native_socket s);

/// The address this socket is actually bound to, which is how a caller learns the port after binding to 0.
[[nodiscard]] cc::result<endpoint, error> local_endpoint(native_socket s);

/// The peer's address on a connected socket.
[[nodiscard]] cc::result<endpoint, error> remote_endpoint(native_socket s);

/// Whether an IPv6 socket also accepts IPv4 traffic.
///
/// Defaulted to v6-only, and set explicitly rather than left alone: the OS default differs by platform and by
/// sysctl, so a caller that does not say gets a different socket on two machines.
[[nodiscard]] cc::result<cc::unit, error> set_v6_only(native_socket s, bool v6_only);

/// Disable Nagle's algorithm.
///
/// Worth having on the socket layer rather than the HTTP one, because a request/response protocol over a Nagled
/// socket pays 40 ms for a small write that follows a small write.
[[nodiscard]] cc::result<cc::unit, error> set_tcp_no_delay(native_socket s, bool no_delay);
} // namespace cnet::impl
