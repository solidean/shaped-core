#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-net/address/endpoint.hh>
#include <clean-net/common/error.hh>
#include <clean-net/io/io_system.hh>
#include <clean-net/transport/backend.hh>

#include <memory> // std::unique_ptr — cc has no polymorphic ownership, so an owned interface uses this repo-wide

/// Datagrams: whole messages to and from any peer, with no connection, no ordering and no delivery promise.
///
/// **Poll-and-batch, not an async per packet.**
/// A game moves small packets 30 to 120 times a second per peer and acts on them inside the same frame, so a packet is
/// a record in a caller-owned array rather than an operation with a state machine and a completion.
/// `receive_batch` and `send_batch` never block and never allocate.
///
/// **Loss, duplication and reordering are data, never errors.**
/// Nothing here fails because a packet did not arrive; an error means the socket itself is unusable or a datagram was
/// refused outright.
///
/// `datagram_socket` is named for what it is rather than for UDP: over `cnet::virtual_network` there is no UDP at all.
/// **Absent on wasm**, where a browser has no datagram socket of any kind; the virtual and simulated ones still work
/// there, so tests built on them run in every build.
/// [docs/datagrams.md](../../../docs/datagrams.md) is the design.

/// One received datagram, in caller-owned memory.
///
/// **`buffer` is the room and is never changed by a receive**, so an array of records is set up once and reused frame
/// after frame; everything else is written by the receive.
struct cnet::datagram
{
    /// Where this datagram may land.
    /// Its size is the most it can hold: a larger datagram is cut there, and `truncated` says so.
    cc::span<byte> buffer;

    /// How many bytes of `buffer` the datagram filled.
    isize size = 0;

    /// Who sent it.
    endpoint peer;

    /// When it arrived, in nanoseconds on the io_system's clock, read as early as the platform allows.
    ///
    /// Today that is right after the receive call returned, so a datagram that waited in the socket's buffer carries
    /// the time it was read rather than the time it arrived.
    /// A simulated link stamps the moment its delay ran out, which is what makes arrival times exact in a test.
    i64 received_at_ns = 0;

    /// The datagram was larger than `buffer`, and the rest of it is gone.
    bool truncated = false;

    /// What arrived.
    [[nodiscard]] cc::span<byte> bytes() const { return buffer.first_n(size); }
};

/// One datagram to send.
struct cnet::outgoing_datagram
{
    endpoint peer;

    /// Must stay alive and unmodified only for the duration of the `send_batch` call.
    cc::span<byte const> bytes;
};

/// What a datagram socket is set up with.
///
/// An option left at its default is not touched, so the socket keeps whatever the OS would have given it.
/// An option a caller set and the OS refuses fails the bind, except `traffic_class`, which is best effort, and
/// `reuse_address` on Windows, which is ignored.
struct cnet::datagram_options
{
    /// For an IPv6 socket, whether it refuses IPv4 traffic.
    /// Set explicitly either way, because the OS default differs by platform and by sysctl.
    bool v6_only = true;

    /// Allow another socket to bind the same port.
    ///
    /// Off, unlike a listener: which of two sockets on one port receives a unicast datagram is then up to the OS,
    /// which is almost never what a caller who did not ask for it wants.
    /// POSIX only; the Windows option of that name permits stealing a port and is never set.
    bool reuse_address = false;

    /// The OS receive buffer, in bytes; 0 keeps the default.
    /// What a burst arriving between two polls has to fit into, so a server with many peers wants it raised.
    i32 receive_buffer_bytes = 0;

    /// The OS send buffer, in bytes; 0 keeps the default.
    i32 send_buffer_bytes = 0;

    /// Refuse to fragment, so a datagram larger than the path allows fails or is dropped rather than split.
    /// What path MTU discovery is built on.
    bool dont_fragment = false;

    /// The IPv4 TOS / IPv6 traffic class byte; 0 leaves it unset.
    /// **Best effort**: Windows ignores it without a QoS policy, and a network is free to rewrite it.
    u8 traffic_class = 0;
};

/// A socket that sends and receives datagrams, whatever is carrying them.
///
/// `receive_batch` may run on several threads at once, and each datagram goes to exactly one of them -- which is what
/// lets a caller poll and keep an arrival callback at the same time.
/// `send_batch` may run beside them; two `send_batch` calls at once are a caller error, since the order of their
/// datagrams relative to each other would be undefined.
class cnet::datagram_socket
{
public:
    /// Bind on the platform's own sockets; `unsupported` on wasm.
    ///
    /// A port of 0 asks the OS to choose one, and `local()` is how you learn which.
    [[nodiscard]] static cc::result<cc::unique_ptr<datagram_socket>, error> try_create(io_system& io,
                                                                                       endpoint const& where,
                                                                                       datagram_options const& options
                                                                                       = {});

    /// Bind on a given transport, which is how a test binds somewhere that is not the network.
    [[nodiscard]] static cc::result<cc::unique_ptr<datagram_socket>, error> try_create(transport& t,
                                                                                       endpoint const& where,
                                                                                       datagram_options const& options
                                                                                       = {});

    /// Throwing counterpart of try_create.
    [[nodiscard]] static cc::unique_ptr<datagram_socket> create(io_system& io,
                                                                endpoint const& where,
                                                                datagram_options const& options = {});

    /// Fill a prefix of `records` with whatever has arrived, and say how long the prefix is.
    ///
    /// Never blocks and never allocates; 0 means nothing was waiting.
    /// Each record's `buffer` is read and never changed, and every other field of the filled ones is overwritten.
    /// An error means the socket itself is unusable, never that a datagram was lost.
    [[nodiscard]] cc::result<isize, error> receive_batch(cc::span<datagram> records);

    /// Send `packets` in order, and say how many were handed on.
    ///
    /// Stops at the first one not taken.
    /// A full OS buffer stops it without an error, and the rest are the caller's to drop or retry.
    /// Any other failure is an error only when it hit the first packet; otherwise the count so far is returned, and the
    /// next call, starting at that packet, reports it.
    /// So an error never hides how much was already sent.
    [[nodiscard]] cc::result<isize, error> send_batch(cc::span<outgoing_datagram const> packets);

    /// Call `callback` whenever datagrams are waiting, until it is replaced or cleared.
    ///
    /// **Level-triggered**: it is told the socket is readable and calls `receive_batch` itself, and if it leaves
    /// datagrams unread it is called again at once.
    /// It runs on the io_system's reactor thread, or on whichever thread drives `cc::thread_pump_all()` when there is
    /// none -- so it must be quick, and handing the datagrams to another thread is its usual job.
    ///
    /// An invalid function clears it.
    /// One that throws is not called again until it is set anew.
    /// Replacing or clearing it, and destroying the socket, wait for a call already running on another thread; from
    /// inside the callback they do not wait.
    /// After `io_system::stop()` it is never called again.
    void on_arrival(cc::unique_function<void(datagram_socket&)> callback);

    /// What the socket actually bound to, port included.
    [[nodiscard]] endpoint local() const;

    explicit datagram_socket(std::unique_ptr<datagram_backend> backend);
    datagram_socket(datagram_socket const&) = delete;
    datagram_socket& operator=(datagram_socket const&) = delete;
    ~datagram_socket();

    /// The callback and the readiness wait behind it, shared with that wait so it can outlive this handle.
    struct arrival_state;

private:
    std::unique_ptr<datagram_backend> _backend;
    cc::shared_ptr<arrival_state> _arrival;
};
