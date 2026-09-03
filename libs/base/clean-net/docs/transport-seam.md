# The transport seam, and simulating a bad network

How a connection stops being "a socket", and what that buys.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## Why this exists

Almost every serious networking bug is a timing or failure bug: a connection that dies mid-body, a server that stalls
after the headers, a response arriving one byte at a time, a request that succeeds on the third retry.
**None of those happen on loopback**, where everything is instant and nothing fails.

So a networking library tested only against a healthy local server is tested against the one condition that never
occurs in production.

The usual remedies are external — a fault-injecting proxy, a kernel traffic shaper, a container with added delay.
All of them are per-platform, need privileges, and cannot run in a unit test.

## The three interfaces

[transport/backend.hh](../src/clean-net/transport/backend.hh) has them, and they are small on purpose.

- **`cnet::connection_backend`** — one end of an established connection: receive, send, half-close, close, and the two
  endpoints.
- **`cnet::listener_backend`** — accept, and what it bound to.
- **`cnet::transport`** — where connections and listeners come from.

`cnet::stream_connection` and `cnet::stream_listener` are handles over the first two, so **every caller writes the same code
whatever is underneath**.
That is the property the seam exists for: a test swaps the transport, not the code being tested.

## The three transports

| | what it is | what it is for |
|---|---|---|
| `cnet::native_transport` | the platform's own sockets | production, and anything that must touch a real network |
| `cnet::virtual_network` | an in-process network, no sockets at all | testing the layers above the transport, fast and deterministic |
| `cnet::simulated_transport` | a wrapper that misbehaves on the way through | testing what happens when the network is bad |

`simulated_transport` composes over either of the others.
Over the virtual one it is a unit test; over the native one it is how you watch the real client behave on a bad link.

## Why the seam sits at the transport rather than only at HTTP

At the HTTP layer a wrapping backend can delay a request and fail it.
It cannot reproduce a connection reset after the headers, or a body arriving one byte at a time, because those are
transport events an HTTP backend has already smoothed over.

The cost of the lower seam is that it only exists where our transport does.
On wasm the backend is the browser's `fetch`, so simulation there will stay at the HTTP layer.
Both seams will exist; neither pretends to cover the other.

## The two properties that separate a simulator from a source of flaky tests

- **Every condition is off by default.**
  A `simulated_transport` nobody configured is indistinguishable from the transport underneath.
  Without that, nobody trusts a passing test either.
- **Every fault is drawn from one seeded stream, and the seed is logged when the link is built.**
  A failing run replays from two numbers: the seed and the conditions.

## How a delay is built

A delay is a reactor `timer` operation — the one operation whose deadline is a success rather than a failure.
It fires on the io_system's clock, so a test with a `cnet::manual_clock` proves a 200 ms link in microseconds instead
of sleeping through it.

The result of the real operation is then forwarded into the promise the caller has been holding, through a one-shot
completion hook rather than through `cc::async`'s compute frames.
Frames need a scheduler; this library deliberately has none, because the reactor is the only place work runs.

## What the virtual network does and does not model

It models a network well enough that code above it cannot tell: listeners at endpoints, refusal where nobody listens,
streams with no message boundaries, half-close, end-of-stream, and real deadlines.

It does not model the parts that would make it slower without making it more honest.
There is no flow control — a send never blocks, because backpressure is `simulated_transport`'s job.
The addresses are make-believe: nothing is checked against the machine's real interfaces.

## Parking, and the races around it

A virtual connection uses the reactor for exactly one thing: **parking**.
A receive with nothing to read, and an accept with nobody knocking, become `manual` operations that the writing side
signals.

That means a submit, a park and a write racing against each other, and getting the order wrong loses a wakeup — which
does not look like a bug, it looks like an operation that waits until its own deadline while the process sits idle.

**Submit before parking.**
Publishing the operation to the pipe first would let a writer signal one the reactor has never seen, and such a signal
is dropped.
The submit and the signal go through the same actor mailbox, so submitting first is what keeps them in order.

**Check and park in one step.**
A writer that appends between "nothing to read" and "parked" finds no reader to signal, and the bytes sit in the pipe
with nobody coming for them.
A receive closes this by doing both under the pipe's own lock — the same lock the writer takes.
An accept has two mutexes and cannot, so it reads `incoming` *inside* `parked_accept`, while the connect side takes
the two one after the other rather than nested.

**Then look again.**
What is left is the gap between submitting and parking, and the answer there is to wake ourselves: exactly one signal
reaches the operation either way.

## Not built yet

A virtual multi-peer network for multiplayer, and the datagram half of all of this.
Their requirements are in [docs/todo/cnet-datagrams.md](../../../../docs/todo/cnet-datagrams.md), and building them
before there is a reliability layer to test would be building against a guess.

