# cnet: datagrams, and a future multiplayer layer

**Status:** the datagram API is decided and not built.
The layer above it is deliberately not designed.

## Two layers, not one

A datagram socket and a "best-effort message channel" make different promises, and separating them is what makes the
second one portable.

- **The datagram layer** is a thin thing over the OS, and **cannot exist in a browser**.
- **The message channel** promises unreliable, unordered, low-latency messages to a peer.
  A browser *can* provide that, over a WebRTC data channel in unreliable mode, which is precisely an unordered lossy
  message pipe.

The upper layer is portable because its promise is *weaker*, not because the lower one was made to work.
That is the same reasoning as the HTTP layering, one level down.

## The datagram API

Poll-and-batch, and **deliberately not the `cc::shared_async` convention the request path uses**.

A game sends and receives small packets 30 to 120 times a second per peer and cares about the latency
*distribution*.
Nothing about that resembles an HTTP request: there is no completion to wait for, no per-packet result, and no error
worth propagating for a packet that was simply lost, because loss is the normal case rather than a failure.

Modelling each packet as an asynchronous operation would give every packet an allocation, a state machine and a
completion callback, to carry sixty bytes the game will act on inside the same frame.

```cpp
namespace cnet
{
struct datagram
{
    endpoint peer;
    cc::span<byte> bytes;      // into caller-owned storage
    i64 received_at_ns = 0;    // as early as the platform allows
};

class udp_socket
{
public:
    /// Fails with `unsupported` on wasm.
    [[nodiscard]] static cc::result<udp_socket, error> try_create(udp_socket_description const& desc = {});

    /// Fill `out` with whatever has arrived. Never blocks, never allocates, returns how many were written.
    /// `storage` is the caller's buffer pool; each datagram's `bytes` points into it.
    [[nodiscard]] i32 receive_batch(cc::span<datagram> out, cc::span<byte> storage);

    /// Send many in as few syscalls as the platform allows. Returns how many were handed to the OS.
    [[nodiscard]] i32 send_batch(cc::span<datagram const> packets);
};
}
```

## Requirements the datagram layer must not foreclose

Getting these wrong is not fixable by optimization; the fix is a different API, by which time a multiplayer layer is
written against the old one.

- **Batched send and receive, caller-owned buffers, no per-packet allocation.**
  One syscall moves many packets — `recvmmsg` / `sendmmsg`, or overlapped receives on Windows.
- **Arrival timestamps taken as early as the platform allows.**
  A jitter buffer is built on when a packet arrived, and a timestamp taken after a queue and a callback is a
  different number.
- **Loss and reordering are data, never errors.**
  Nothing produces a failure for an ordinary lost packet.
- **Per-socket options are reachable:** don't-fragment, traffic class, buffer sizes.
- **The socket type is an interface** a simulated or virtual implementation can stand in for
  ([cnet-transport-seam.md](cnet-transport-seam.md)).
  A multiplayer layer is nearly untestable without one: latency, jitter, loss, duplication and reordering are what
  its logic exists to survive.

## Requirements a portable message channel must not foreclose

These four are the ones a desktop-first design gets wrong by default, and they are expensive to retrofit:

- **Peer identity is an opaque id, not an `endpoint`.**
  A browser peer has no address we can name.
- **Connecting to a peer is asynchronous, slow, and can fail** for reasons that have nothing to do with the peer.
- **Signalling is out of scope and pluggable.**
  The layer needs session data exchanged; how it travels is the application's business.
- **MTU is reported by the channel**, never assumed by the caller.
- **Reliability and ordering are per-message options**, because SCTP offers them per stream and a native
  implementation would offer them per channel.

## The WebRTC path is a project, not a backend

It brings signalling, ICE, STUN and DTLS-SCTP with it, plus connection setup measured in hundreds of milliseconds.
Naming it as in-scope is right; costing it as "one more backend" would not be.

## Where the reliability layer lives

Undecided, deliberately.
It belongs above this library, and building it before something needs it would be building against a guess.
What is settled is the list above — the shape the layer below it has to have.
