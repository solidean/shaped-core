# cnet: the backend seam, and simulating a bad network

**Status:** decided, not built.
Agreed to land in the first slice, kept simple, with the API surface complete.

## Why this exists

Almost every serious networking bug is a timing or failure bug: a connection that dies mid-body, a server that stalls
after the headers, a response arriving one byte at a time, a request that succeeds on the third retry.
**None of those happen on loopback**, where everything is instant and nothing fails.

So a networking library tested only against a healthy local server is tested against the one condition that never
occurs in production.

The usual remedies are external — a fault-injecting proxy, a kernel traffic shaper, a container with added delay.
All of them are per-platform, need privileges, and cannot run in a unit test.

## Two different things, both wanted

**A wrapping backend** takes any other backend and misbehaves on the way through: delay, jitter, a bandwidth cap,
truncated bodies, a connection reset at a chosen byte offset, a `503` on the first N attempts, a DNS failure.
It is thin, it composes with whatever is underneath, and it turns "does the retry policy work" into a test.
It also works over a *real* backend, which is how you watch the real client behave on a bad link.

**A virtual backend** has no network at all: requests are answered by a handler in this process, with no socket, no
reactor and no loopback.
It is what makes a test of *caller* logic — a downloader, an asset pipeline, a live `cc::rec` view — fast and
completely deterministic.

## The decision that matters: which layer the seam sits at

**Both layers**, and the lower one is the one that earns its keep.

At the HTTP layer a wrapping backend can delay a request and fail it.
It cannot reproduce a connection reset after the headers, or a body arriving one byte at a time, because those are
transport events the HTTP backend has already smoothed over.

At the transport layer it reproduces all of it — and it also serves the datagram path
([cnet-datagrams.md](cnet-datagrams.md)), where loss and reordering are the *normal* case and are the single hardest
thing to test.

The cost of the lower seam is that it only exists where our transport does.
On wasm the backend is the browser's `fetch`, so simulation there stays at the HTTP layer.
Both seams exist; neither pretends to cover the other.

## What has to be built

The API surface is settled:

```cpp
namespace cnet
{
/// What a link does to traffic passing through it. Every field is off by default.
struct link_conditions
{
    f32 latency_ms = 0;
    f32 jitter_ms = 0;
    f32 loss_probability = 0;       // datagrams dropped, or a stream reset
    f32 duplicate_probability = 0;  // datagram paths only
    f32 reorder_probability = 0;    // datagram paths only
    f32 bandwidth_bytes_per_sec = 0;

    /// Cut a stream connection after this many bytes of body. 0 = never.
    isize reset_after_bytes = 0;

    /// Everything above is driven from this seed, so a failing run replays exactly.
    u64 seed = 0;
};
}
```

Two properties are what separate a useful simulator from a source of flaky tests, and both were agreed:

- **Every condition is off by default**, so a simulated backend with default settings is indistinguishable from the
  real one.
  Without that, nobody trusts a passing test either.
- **Fault schedules are seeded and the seed is logged**, so a failing run replays from two numbers.
  Use `clean-core/math/random.hh`, and pair it with the injectable clock the reactor already has.

## What the transport has to become

Today `cnet::tcp_connection` and `cnet::tcp_listener` call `impl::native_socket` functions directly.
For a virtual implementation to stand in, the operations have to reach their backend through an interface rather than
through free functions.

**This is the part that is expensive to retrofit and cheap to do now**, which is why it was agreed for the first
slice: a socket type written as a concrete class has to be rewritten, not extended.

## Not in the first version

A virtual multi-peer network for multiplayer.
Its requirements belong in [cnet-datagrams.md](cnet-datagrams.md), and building it before there is a reliability
layer to test would be building against a guess.
