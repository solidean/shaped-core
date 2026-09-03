# clean-net structure

What exists today, what is planned, and what is deliberately out of scope.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## The layering

Three pieces, each reporting its own availability, rather than one tower.
The [readme](../readme.md#the-one-thing-to-know-first) says why: a browser has no sockets and does have HTTP.

- **transport** — sockets, the reactor, datagrams.
  Absent on wasm, though its seam is not: a virtual or simulated transport needs no sockets.
- **protocol clients** — HTTP and WebSocket, over a backend.
  Present everywhere, at different capability levels.
- **listeners** — the server side.
  Absent on wasm.

## Support matrix

| | wasm | Windows | Linux | macOS | iOS / Android |
|---|---|---|---|---|---|
| TCP | — | **done** | done, unverified | done, unverified | planned |
| virtual + simulated transports | done | done | done | done | done |
| UDP datagrams | — | planned | planned | planned | planned |
| listeners | — | **done** | done, unverified | done, unverified | planned |
| HTTP server (loopback) | — | **done** | done, unverified | done, unverified | planned |
| HTTP client | planned (`fetch`) | **done** (native) | done, unverified | done, unverified | planned |
| WebSocket (client + server) | planned (browser) | **done** (native) | done, unverified | done, unverified | planned |
| TLS | browser's | **done** | done, unverified | done, unverified | Android: no trust store |
| name resolution | — (the browser resolves inside `fetch`) | **done** | done, unverified | done, unverified | planned |

A dash is `error_code::unsupported`: the platform has no such concept and never will, so a caller decides once at
startup rather than probing per call.

## Status

**[done]** the vocabulary — `cnet::error` and its codes, `cnet::deadline`, the `cnet::clock` seam and its manual
counterpart, `cnet::ip_address` and `cnet::endpoint`, and the `cnet::http_level` ladder.
None of it touches the network, so all of it is testable without one.

**[done]** the reactor, `cnet::io_system`, and TCP.
Connect, accept, send, receive and half-close as `cc::shared_async`, with deadlines the reactor enforces against the
injected clock.
"Done, unverified" above means the same code path Windows runs, on a platform nobody has run it on yet.

**[done]** WebSockets, client and server.
`cnet::websocket_connect` performs the handshake and hands back a message channel; `http_server::websocket_route`
upgrades a path instead of answering it.
Framing is strict, control frames are answered here, and both ends run over the virtual network in one process.
[websockets.md](websockets.md) is the design.

**[done]** the loopback dev server.
Routes, static files confined under their root, streamed responses over chunked encoding, keep-alive, the limits that
keep it from being trivially crashable, and shutdown through its own token.
Not hardened for hostile input, on purpose; [http.md](http.md#the-server) says what that means and what it refuses to
grow into.

**[done]** the HTTP client, over our own transport.
`cnet::http_target` over `cc::uri`, the HTTP/1.1 wire format, and a client that resolves, connects, handshakes,
sends and parses -- with streaming, backpressure, redirects, connection pooling, per-host politeness and retries,
and one budget for all of it.
[http.md](http.md) is the design.

**[done]** TLS, over a vendored Mbed TLS.
`cnet::tls_connect` and `cnet::tls_accept` wrap any connection, so a handshake runs over the virtual network with no
socket in sight -- which is where every TLS test here runs.
Windows, macOS and Linux read the machine's own roots; iOS and Android report `unsupported` rather than an empty
set, because neither can enumerate anchors at all -- both want the OS to evaluate a chain instead, which is a
different seam.
[tls.md](tls.md) is the design.

**[done]** name resolution, and the race above it.
`cnet::resolver` runs a blocking `getaddrinfo` on a worker thread behind a TTL cache, and `cnet::connect_to_host`
resolves and connects as one operation, racing the families so a broken IPv6 route costs milliseconds rather than a
timeout.
[name-resolution.md](name-resolution.md) is the design.

**[done]** cancellation.
`cnet::cancel_token` groups the operations of one request the way a deadline bounds each of them, and a cancelled
outcome arrives as `cc::async_error::is_cancelled()`.
[cancellation.md](cancellation.md) is the design.

**[done]** the transport seam and the two transports that stand in for a network:
`cnet::virtual_network` answers in this process over no socket, and `cnet::simulated_transport` delays, drops and cuts
on the way through to another one.
[transport-seam.md](transport-seam.md) is the design.

**[planned]**, roughly in the order it will be built:

1. the `fetch` and browser-`WebSocket` backends for wasm, and a `dlopen`ed system libcurl where one is present;
2. UDP datagrams, in the poll-and-batch shape [docs/todo/cnet-datagrams.md](../../../../docs/todo/cnet-datagrams.md) records.

## What clean-core is missing that this library wants

**Path resolution and directory metadata.**
clean-core says plainly that it is not a filesystem layer — no `mkdir`, no iteration, no metadata, no path arithmetic
— and `serve_directory` is written around that: it refuses every escape token lexically rather than resolving a path
and comparing it to the root.
That is the stronger check for what it covers, and it does not cover a symlink under the root pointing outside it.
Streaming a large file rather than reading it whole wants the same surface.

## Deliberately out of scope

- **HTTP/2 and HTTP/3.** HPACK, stream multiplexing and QUIC are each a project.
  Wanting HTTP/2 is a reason to select a system backend, not a reason to write one.
- **A hardened web server.** Slowloris, request smuggling, and a threat model where every byte is attacker-controlled
  are the difference between a dev server and a product, and this is the former.
- **Bandwidth shaping and server-side rate limiting.** Client politeness is in; the other two mechanisms that share
  the words "rate limit" are not.
- **A multiplayer reliability layer.**
  [docs/todo/cnet-datagrams.md](../../../../docs/todo/cnet-datagrams.md) records the requirements the datagram layer
  must not foreclose, and the layer itself belongs above this library when something needs it.

## The reactor waits with `select` and `poll`, not IOCP and epoll

One shared readiness poller behind a completion-shaped interface — `select` on Windows, `poll` elsewhere.
IOCP and epoll are both better at scale and both drop in behind that interface without touching a line above it.
What they are not is *shared*: they have no code in common, so whichever platform is not in front of you ships
unverified.

The cost is written down rather than discovered: both pollers are O(n) in pending operations per wait, and `select`
watches at most `FD_SETSIZE` sockets.

**Past that cap a wait starts from where the last one stopped**, rather than from the front — otherwise the tail of the
list is never watched at all, which is a connection starved rather than slow.
A wait that had to leave sockets out also parks only briefly, since one of the sockets it left out may be ready and
nothing will say so.
Fine for a dev server and a pooled HTTP client; wrong for ten thousand connections, which is the point at which
replacing that one file earns its keep.

`select` rather than `WSAPoll` on Windows is a correctness choice rather than a scale one.
WSAPoll does not report a failed connection at all, so a refused connect would hang to its deadline instead of
failing — a known, unfixed defect that curl documents and works around.

## The clock is a seam from the first version

Timeouts, retries, rate limits and connection races are all clock-dependent, and testing them with real sleeps is both
slow and flaky.
`cnet::clock` exists so the reactor reads time from something a test can move, and `cnet::manual_clock` is that
something.
It is here now rather than later because retrofitting one means touching every place that reads a clock.

## The reactor runs more than sockets

Two operation kinds wait on nothing the OS knows about.
A **timer** completes successfully once the clock passes its deadline, which is what a retry backoff, a happy-eyeballs
head start and a simulated link's latency are all made of.
A **manual** operation completes when its owner signals it, and times out if nobody does — which is how a virtual
connection, and later a browser `fetch`, joins the same async machinery instead of growing a second one.

That is also why an `io_system` exists on a platform with no sockets at all: a request served by `fetch` still has a
deadline, and the deadline is the reactor's.
