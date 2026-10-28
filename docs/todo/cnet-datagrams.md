# cnet: datagrams, and a future multiplayer layer

**Status:** the datagram layer is built — [libs/base/clean-net/docs/datagrams.md](../../libs/base/clean-net/docs/datagrams.md) is its design.
What remains here is the layer above it, which is deliberately not designed.

## Two layers, not one

A datagram socket and a "best-effort message channel" make different promises, and separating them is what makes the
second one portable.

- **The datagram layer** is a thin thing over the OS, and **cannot exist in a browser**.
- **The message channel** promises unreliable, unordered, low-latency messages to a peer.
  A browser *can* provide that, over a WebRTC data channel in unreliable mode, which is precisely an unordered lossy
  message pipe.

The upper layer is portable because its promise is *weaker*, not because the lower one was made to work.
That is the same reasoning as the HTTP layering, one level down.

## The datagram layer

Built: `cnet::datagram_socket`, polled in batches over caller-owned records, over the native, virtual and simulated transports.
Its shape, and how it meets the requirements this file used to list for it, is [datagrams.md](../../libs/base/clean-net/docs/datagrams.md).

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
