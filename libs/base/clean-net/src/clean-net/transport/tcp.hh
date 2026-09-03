#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/address/endpoint.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>
#include <clean-net/impl/native_socket.hh>
#include <clean-net/io/io_system.hh>

/// TCP, as the transport layer a protocol is written over.
///
/// Every operation returns a `cc::shared_async`, so it composes with everything else in shaped-core that is async,
/// and cancellation arrives already modelled on `cc::async_error`.
/// **Nothing here requires blocking to obtain a result**, which is what keeps a browser main thread and a render
/// thread first-class callers.
///
/// **Absent on wasm**, where a program cannot open a socket at all.
/// Every factory reports `error_code::unsupported` there rather than the types disappearing.
///
/// A connection is held by `cc::shared_ptr` rather than uniquely, because an operation in flight refers to its
/// socket: shared ownership is what makes dropping the last handle mid-read safe instead of a use-after-free.

/// What a TCP socket is set up with.
struct cnet::tcp_options
{
    /// Disable Nagle's algorithm.
    ///
    /// On by default, unlike the OS: a request/response protocol over a Nagled socket pays about 40 ms whenever a
    /// small write follows a small write, and that is the shape of nearly everything above this layer.
    bool no_delay = true;

    /// For an IPv6 socket, whether it refuses IPv4 traffic.
    ///
    /// Set explicitly either way, because the OS default differs by platform and by sysctl.
    bool v6_only = true;
};

/// What a listener is opened with.
struct cnet::tcp_listen_options
{
    tcp_options socket;

    /// How many established connections the OS may hold before the program accepts them.
    i32 backlog = 128;

    /// Allow binding a port left in TIME_WAIT by a previous process.
    ///
    /// POSIX only, and deliberately so: the same-named Windows option permits *stealing* a port another socket
    /// currently holds, which is a different and much worse thing.
    bool reuse_address = true;
};

/// An established TCP connection.
///
/// Reads and writes may be in flight at the same time; two reads at once are a caller error, since the second would
/// take bytes the first was promised.
class cnet::tcp_connection
{
public:
    /// Read into `buffer`.
    ///
    /// Completes on the FIRST bytes that arrive and reports how many, which may be far fewer than the buffer holds:
    /// a stream has no message boundaries, so waiting to fill the buffer would be waiting for a message nobody sent.
    /// A peer that closed cleanly fails with `connection_closed` rather than reporting zero bytes.
    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d = deadline::after_secs(30));

    /// Write all of `bytes`.
    ///
    /// Completes only once every byte has been handed to the OS, since a partial write is never what a caller meant.
    /// `bytes` must stay alive and unmodified until it completes.
    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes, deadline d = deadline::after_secs(30));

    [[nodiscard]] endpoint local() const { return _local; }
    [[nodiscard]] endpoint peer() const { return _peer; }

    /// Whether the socket is still open.
    /// False after `close`, and after a failure that took the connection down with it.
    [[nodiscard]] bool is_open() const;

    /// Stop using this connection.
    ///
    /// The socket itself closes once no operation still refers to it, which is usually at once and is not promised:
    /// a handle closed while the reactor watches it can be reissued to the next socket the process opens, and the
    /// reactor would then be watching a stranger.
    /// An operation already in flight therefore runs to its own end -- its deadline, or the peer -- rather than being
    /// cut short here.
    void close();

    tcp_connection(io_system& io, cc::shared_ptr<impl::socket_holder> s, endpoint local_endpoint, endpoint peer_endpoint);
    tcp_connection(tcp_connection const&) = delete;
    tcp_connection& operator=(tcp_connection const&) = delete;
    ~tcp_connection();

private:
    io_system& _io;
    cc::shared_ptr<impl::socket_holder> _socket;
    endpoint _local;
    endpoint _peer;
};

/// A socket accepting inbound connections.
///
/// **Absent on wasm**: a program there cannot listen at all, so `try_create` reports `unsupported`.
class cnet::tcp_listener
{
public:
    /// Bind and listen.
    ///
    /// A port of 0 asks the OS to choose one, and `local()` is how you learn which -- the normal thing for a test
    /// server, and what keeps two of them from colliding.
    [[nodiscard]] static cc::result<cc::unique_ptr<tcp_listener>, error> try_create(io_system& io,
                                                                                    endpoint const& where,
                                                                                    tcp_listen_options const& options
                                                                                    = {});

    /// Throwing counterpart of try_create.
    [[nodiscard]] static cc::unique_ptr<tcp_listener> create(io_system& io,
                                                             endpoint const& where,
                                                             tcp_listen_options const& options = {});

    /// Take the next inbound connection.
    ///
    /// The default is no deadline, which is what a server wants: a listener waiting for its next client is idle
    /// rather than late.
    [[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> accept(deadline d = deadline::never());

    /// What the listener actually bound to, port included.
    [[nodiscard]] endpoint local() const { return _local; }

    tcp_listener(io_system& io, cc::shared_ptr<impl::socket_holder> s, endpoint local_endpoint);
    tcp_listener(tcp_listener const&) = delete;
    tcp_listener& operator=(tcp_listener const&) = delete;
    ~tcp_listener();

private:
    io_system& _io;
    cc::shared_ptr<impl::socket_holder> _socket;
    endpoint _local;
    tcp_options _options;
};

namespace cnet
{
/// Connect to `where`.
///
/// One deadline covers the whole attempt rather than each step of it.
/// Fails with `unsupported` where the platform has no sockets, and with `connection_refused`, `host_unreachable` or
/// `timed_out` for the ordinary reasons.
///
/// This takes an address rather than a name: resolving a name can block and needs the OS, so it is `cnet::resolve`'s
/// job and never a connect's.
[[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> tcp_connect(io_system& io,
                                                                           endpoint const& where,
                                                                           deadline d = deadline::after_secs(30),
                                                                           tcp_options const& options = {});
} // namespace cnet
