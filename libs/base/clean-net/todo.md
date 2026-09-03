# clean-net todo

Agreed in design but not built.
The [structure.md](docs/structure.md) roadmap is what is planned; this is what is *decided* and waiting.

## Requirements the datagram layer must not foreclose

A multiplayer reliability layer belongs above this library, and it does not exist yet.
What is settled is the shape the layer below it has to have, because getting these wrong is not fixable by
optimization later:

- **Batched send and receive, with caller-owned buffers and no per-packet allocation.**
  One syscall moves many packets — `recvmmsg` / `sendmmsg`, or overlapped receives on Windows.
- **Arrival timestamps taken as early as the platform allows.**
  A jitter buffer is built on when a packet arrived, and a timestamp taken after a queue and a callback is a
  different number.
- **Loss and reordering are data, never errors.** Nothing produces a failure for an ordinary lost packet.
- **Per-socket options are reachable**: don't-fragment, traffic class, buffer sizes.
- **The socket type is an interface** a simulated or virtual implementation can stand in for, which is what makes a
  reliability layer testable at all.

## Requirements a portable message channel must not foreclose

The datagram layer cannot exist on wasm, and a best-effort message channel can — a WebRTC data channel in unreliable
mode is an unordered lossy message pipe.
That is a project rather than a backend swap, since it brings signalling, ICE, STUN and DTLS-SCTP with it.
What a future channel API must assume from the start:

- **Peer identity is an opaque id, not an `endpoint`.** A browser peer has no address we can name.
- **Connecting to a peer is asynchronous, slow, and can fail** for reasons that have nothing to do with the peer.
- **Signalling is out of scope and pluggable.** The layer needs session data exchanged; how it travels is the
  application's business.
- **MTU is reported by the channel**, never assumed by the caller.
- **Reliability and ordering are per-message options.**

## Smaller things

- Declare a precompiled-header tier once there is enough here to measure one with `uv run dev.py compile-time pch`.
  Picking a tier by eye is how a bigger one gets chosen than the numbers support.
- A `network.py` beside `tools/dev/lib/toolchain/graphics.py`, so `uv run dev.py doctor` names the TLS backend and the
  HTTP backends this machine offers — advisory, never a failure.
