#include "reactor.hh"

#include <clean-core/string/format.hh>

// The platform half of the reactor: one poller, two spellings -- `select` on Windows and `poll` everywhere else.
// Everything that is not a socket -- the pending list, deadlines, cancellation, timers -- is in reactor.cc.
//
// WHY NOT IOCP AND EPOLL, which is where a serious reactor ends up.
// Both are strictly better at scale, and both drop in behind this file without touching a line above it -- which is
// what the completion-shaped interface in reactor.hh is for.
// What they are not is *shared*: an IOCP path and an epoll path have no code in common, so whichever platform is not
// in front of you would be carried unverified.
// One shared path means Linux and macOS run the same logic Windows does, and the platform-specific surface is this
// one file.
//
// WHY `select` ON WINDOWS RATHER THAN `WSAPoll`, which looks like the modern choice.
// WSAPoll does not report a failed connection: a refused connect is reported in neither the readable nor the error
// set, so the operation would hang until its deadline instead of failing with connection_refused.
// It is a known, unfixed defect that curl documents and works around.
// `select` reports a failed connect in its exception set, which is what the connect path below reads.
//
// WHAT THIS COSTS.
// Both pollers are O(n) in the number of pending operations per wait, and `select` on Windows watches at most
// FD_SETSIZE sockets.
// That is fine for a dev server and an HTTP client with a connection pool, and it is not fine for ten thousand
// connections -- which is the point at which replacing this file earns its keep.

#ifndef CNET_HAS_SOCKETS
#define CNET_HAS_SOCKETS 0
#endif

#if CNET_HAS_SOCKETS

#if defined(_WIN32)
// clang-format off
// FD_SETSIZE must be defined before winsock2.h, which sizes fd_set from it; the default of 64 is far too small.
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace cnet::impl
{
namespace
{
#if defined(_WIN32)
using raw_socket = SOCKET;
constexpr isize k_max_watched = FD_SETSIZE;
#else
using raw_socket = int;
constexpr isize k_max_watched = 4096;
#endif

[[nodiscard]] raw_socket raw_of(native_socket s)
{
    return raw_socket(s);
}

/// Whether the platform is saying "not yet" rather than "no".
[[nodiscard]] bool is_would_block(i32 native)
{
#if defined(_WIN32)
    return native == WSAEWOULDBLOCK || native == WSAEINPROGRESS;
#else
    return native == EWOULDBLOCK || native == EAGAIN || native == EINPROGRESS || native == EINTR;
#endif
}

/// The pending error a connecting socket carries once it is done, 0 when it succeeded.
[[nodiscard]] i32 pending_socket_error(native_socket s)
{
    int value = 0;
#if defined(_WIN32)
    int length = int(sizeof(value));
#else
    socklen_t length = sizeof(value);
#endif
    if (::getsockopt(raw_of(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &length) != 0)
        return last_socket_error();
    return i32(value);
}

/// Whether an operation is waiting to read, to write, or neither -- a timer and a manual operation watch nothing.
struct watch
{
    bool read = false;
    bool write = false;
};

[[nodiscard]] watch watch_of(io_op_kind kind)
{
    switch (kind)
    {
    case io_op_kind::connect:
    case io_op_kind::send:
        return {.read = false, .write = true};
    case io_op_kind::accept:
    case io_op_kind::receive:
        return {.read = true, .write = false};
    case io_op_kind::timer:
    case io_op_kind::manual:
        return {};
    }
    return {};
}

[[nodiscard]] error make_error(error_code code, cc::string message)
{
    return {.code = code, .native_code = 0, .message = cc::move(message)};
}
} // namespace

cc::result<native_socket, error> reactor::create_wake_channel()
{
    // The wake channel is a UDP socket connected to itself: one mechanism on every platform, needing no pipe, no
    // eventfd, and no second handle type for the poller to understand.
    auto wake = create_udp_socket(ip_family::v4);
    if (wake.has_error())
        return cc::error(cc::move(wake).error());

    auto const s = wake.value();
    auto bound = bind_socket(s, endpoint(ip_address::loopback(ip_family::v4), 0), false);
    if (bound.has_error())
    {
        close_socket(s);
        return cc::error(cc::move(bound).error());
    }

    auto local = local_endpoint(s);
    if (local.has_error())
    {
        close_socket(s);
        return cc::error(cc::move(local).error());
    }

    auto connected = connect_socket(s, local.value());
    if (connected.has_error())
    {
        close_socket(s);
        return cc::error(cc::move(connected).error());
    }

    return s;
}

isize reactor::max_watched()
{
    return k_max_watched;
}

void reactor::wake()
{
    if (_wake_socket == k_invalid_socket)
        return;

    // One byte in flight is enough to end a wait, so a burst of wakes costs one send rather than one each.
    if (_wake_pending.exchange(true))
        return;

    char const one = 1;
    (void)::send(raw_of(_wake_socket), &one, 1, 0);
}

void reactor::drain_wake()
{
    // THE ORDER HERE IS THE INTERESTING PART, and the other one is a hang.
    //
    // A `wake()` landing between these two lines sees the flag still set, sends nothing, and is lost -- so the reactor
    // parks with work waiting, until the next wake or the caller's own wait cap ends it.
    // A bounded latency, and self-correcting: the flag is false by then, so the next wake gets through.
    //
    // Clearing the flag FIRST instead would trade that for a permanent one: the wake in the window would send its byte
    // into a drain that is still running, the byte would be swallowed, and the flag would stay set with nothing in
    // flight -- suppressing every wake from then on.
    drain_datagrams(_wake_socket);
    _wake_pending.store(false);
}

cc::optional<cc::optional<error>> reactor::drive_socket(entry& e)
{
    auto* const op = e.op;
    switch (op->kind)
    {
    case io_op_kind::connect:
    {
        if (!e.writable && !e.errored)
            return {};

        auto const native = pending_socket_error(op->socket);
        if (native != 0)
            return cc::optional<error>(error_from_native(native, cc::format("connecting to {}", op->peer)));
        return cc::optional<error>();
    }

    case io_op_kind::accept:
    {
        if (!e.readable)
            return {};

        auto accepted = accept_socket(op->socket);
        if (accepted.has_error())
        {
            // The connection can go away between readiness and accept, which is normal rather than a failure.
            if (is_would_block(accepted.error().native_code))
                return {};
            return cc::optional<error>(cc::move(accepted).error());
        }
        op->accepted = accepted.value();
        return cc::optional<error>();
    }

    case io_op_kind::receive:
    {
        if (!e.readable)
            return {};

        auto const n = ::recv(raw_of(op->socket), reinterpret_cast<char*>(op->buffer), int(op->buffer_size), 0);
        if (n > 0)
        {
            op->transferred = isize(n);
            return cc::optional<error>();
        }
        if (n == 0)
            return cc::optional<error>(make_error(error_code::connection_closed, cc::string("the peer closed the "
                                                                                            "connection")));

        auto const native = last_socket_error();
        if (is_would_block(native))
            return {};
        return cc::optional<error>(error_from_native(native, "receiving"));
    }

    case io_op_kind::send:
    {
        if (!e.writable)
            return {};

        // Loop until the kernel pushes back: a partial send is the normal case on a full socket buffer, and the
        // retry belongs here rather than in every caller.
        while (op->transferred < op->buffer_size)
        {
            auto const remaining = op->buffer_size - op->transferred;
            auto const n = ::send(raw_of(op->socket), reinterpret_cast<char const*>(op->buffer + op->transferred),
                                  int(remaining), 0);
            if (n > 0)
            {
                op->transferred += isize(n);
                continue;
            }

            auto const native = last_socket_error();
            if (is_would_block(native))
                return {};
            return cc::optional<error>(error_from_native(native, "sending"));
        }
        return cc::optional<error>();
    }

    case io_op_kind::timer:
    case io_op_kind::manual:
        return {}; // never routed here -- reactor::drive answers these itself
    }
    return {};
}

#if defined(_WIN32)
void reactor::poll_once(i32 timeout_ms)
{
    fd_set read_set;
    fd_set write_set;
    fd_set except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

    for (auto& e : _pending)
    {
        e.readable = false;
        e.writable = false;
        e.errored = false;
    }

    isize watched = 0;
    if (_wake_socket != k_invalid_socket)
    {
        FD_SET(raw_of(_wake_socket), &read_set);
        ++watched;
    }

    // From the cursor rather than from the front, so that past the cap the tail is watched on a later round instead of
    // never.
    auto const count = _pending.size();
    auto scanned = isize(0);
    for (; scanned < count; ++scanned)
    {
        auto& e = _pending[(_watch_cursor + scanned) % count];

        if (e.cancelled || e.immediate_failure.has_value())
            continue;
        if (e.op->socket == k_invalid_socket)
            continue; // a timer or a manual operation has nothing to watch
        if (watched >= k_max_watched)
            break; // the rest wait for the next round rather than overrunning the set

        auto const w = watch_of(e.op->kind);
        if (w.read)
            FD_SET(raw_of(e.op->socket), &read_set);
        if (w.write)
        {
            FD_SET(raw_of(e.op->socket), &write_set);

            // A refused connect is reported here and nowhere else, which is the whole reason this is select.
            if (e.op->kind == io_op_kind::connect)
                FD_SET(raw_of(e.op->socket), &except_set);
        }
        ++watched;
    }

    if (count > 0)
        _watch_cursor = (_watch_cursor + scanned) % count;

    if (watched == 0)
        return;

    timeval tv = {};
    timeval* tv_ptr = nullptr;
    if (timeout_ms >= 0)
    {
        tv.tv_sec = long(timeout_ms / 1000);
        tv.tv_usec = long((timeout_ms % 1000) * 1000);
        tv_ptr = &tv;
    }

    // select ignores its first argument on Windows, where an fd_set is a counted array rather than a bitmap.
    if (::select(0, &read_set, &write_set, &except_set, tv_ptr) <= 0)
        return;

    if (_wake_socket != k_invalid_socket && FD_ISSET(raw_of(_wake_socket), &read_set))
        drain_wake();

    for (auto& e : _pending)
    {
        if (e.op->socket == k_invalid_socket)
            continue;

        e.readable = FD_ISSET(raw_of(e.op->socket), &read_set) != 0;
        e.writable = FD_ISSET(raw_of(e.op->socket), &write_set) != 0;
        e.errored = FD_ISSET(raw_of(e.op->socket), &except_set) != 0;
    }
}
#else
void reactor::poll_once(i32 timeout_ms)
{
    auto fds = cc::vector<pollfd>();
    auto indices = cc::vector<isize>();
    fds.reserve_back(_pending.size() + 1);
    indices.reserve_back(_pending.size());

    for (auto& e : _pending)
    {
        e.readable = false;
        e.writable = false;
        e.errored = false;
    }

    if (_wake_socket != k_invalid_socket)
        fds.push_back({.fd = raw_of(_wake_socket), .events = POLLIN, .revents = 0});

    // From the cursor rather than from the front, so that past the cap the tail is watched on a later round instead of
    // never.
    auto const count = _pending.size();
    auto scanned = isize(0);
    for (; scanned < count; ++scanned)
    {
        auto const i = (_watch_cursor + scanned) % count;
        auto& e = _pending[i];

        if (e.cancelled || e.immediate_failure.has_value())
            continue;
        if (e.op->socket == k_invalid_socket)
            continue; // a timer or a manual operation has nothing to watch
        if (fds.size() >= k_max_watched)
            break;

        auto const w = watch_of(e.op->kind);
        short events = 0;
        if (w.read)
            events = short(events | POLLIN);
        if (w.write)
            events = short(events | POLLOUT);

        fds.push_back({.fd = raw_of(e.op->socket), .events = events, .revents = 0});
        indices.push_back(i);
    }

    if (count > 0)
        _watch_cursor = (_watch_cursor + scanned) % count;

    if (fds.empty())
        return;

    if (::poll(fds.data(), nfds_t(fds.size()), timeout_ms) <= 0)
        return;

    isize first = 0;
    if (_wake_socket != k_invalid_socket)
    {
        if ((fds[0].revents & POLLIN) != 0)
            drain_wake();
        first = 1;
    }

    for (isize k = 0; k < indices.size(); ++k)
    {
        auto const& p = fds[first + k];
        auto& e = _pending[indices[k]];

        // POLLERR and POLLHUP arrive unrequested, and a connecting socket that failed reports one of them.
        // They are folded into both directions so `drive` runs and reads the real reason out of SO_ERROR.
        auto const failed = (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        e.readable = (p.revents & POLLIN) != 0 || failed;
        e.writable = (p.revents & POLLOUT) != 0 || failed;
        e.errored = failed;
    }
}
#endif
} // namespace cnet::impl

#else // CNET_HAS_SOCKETS -- no sockets, so timers and manual operations are all there is to run

namespace cnet::impl
{
cc::result<native_socket, error> reactor::create_wake_channel()
{
    // No socket to wake with, and none needed: a reactor here never parks, because io_system runs it unthreaded.
    return k_invalid_socket;
}

isize reactor::max_watched()
{
    // Nothing is ever watched, so the cap never binds.
    return 0;
}

void reactor::wake()
{
}

void reactor::drain_wake()
{
}

void reactor::poll_once(i32)
{
}

cc::optional<cc::optional<error>> reactor::drive_socket(entry&)
{
    return cc::optional<error>(unsupported_here("a socket operation"));
}
} // namespace cnet::impl

#endif // CNET_HAS_SOCKETS
