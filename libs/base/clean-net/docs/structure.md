# clean-net structure

What exists today, what is planned, and what is deliberately out of scope.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## The layering

Three pieces, each reporting its own availability, rather than one tower.
The [readme](../readme.md#the-one-thing-to-know-first) says why: a browser has no sockets and does have HTTP.

- **transport** — sockets, the reactor, datagrams.
  Absent on wasm.
- **protocol clients** — HTTP and WebSocket, over a backend.
  Present everywhere, at different capability levels.
- **listeners** — the server side.
  Absent on wasm.

## Support matrix

| | wasm | Windows | Linux | macOS | iOS / Android |
|---|---|---|---|---|---|
| TCP | — | planned | planned | planned | planned |
| UDP datagrams | — | planned | planned | planned | planned |
| listeners | — | planned | planned | planned | planned |
| HTTP client | planned (`fetch`) | planned (native) | planned (native, system curl) | planned (native) | planned |
| WebSocket client | planned (browser) | planned (native) | planned (native) | planned (native) | planned |
| TLS | browser's | planned (mbedTLS) | planned (mbedTLS) | planned (mbedTLS) | planned |
| name resolution | — (the browser resolves inside `fetch`) | planned | planned | planned | planned |

A dash is `error_code::unsupported`: the platform has no such concept and never will, so a caller decides once at
startup rather than probing per call.

## Status

**[done]** the vocabulary — `cnet::error` and its codes, `cnet::deadline`, the `cnet::clock` seam and its manual
counterpart, `cnet::ip_address` and `cnet::endpoint`, and the `cnet::http_level` ladder.
None of it touches the network, so all of it is testable without one.

**[planned]**, roughly in the order it will be built:

1. the reactor and TCP — IOCP and epoll against one internal seam, kqueue and a wasm null beside them;
2. the transport backend seam, with the simulated and virtual backends over it;
3. name resolution — thread-offloaded `getaddrinfo` behind a cache, with happy eyeballs above it;
4. TLS over a vendored mbedTLS, with per-platform trust stores;
5. the HTTP/1.1 native backend, the client seam and the convenience calls;
6. the `fetch` backend for wasm, and a `dlopen`ed system libcurl where one is present;
7. the loopback server and WebSocket.

## Deliberately out of scope

- **HTTP/2 and HTTP/3.** HPACK, stream multiplexing and QUIC are each a project.
  Wanting HTTP/2 is a reason to select a system backend, not a reason to write one.
- **A hardened web server.** Slowloris, request smuggling, and a threat model where every byte is attacker-controlled
  are the difference between a dev server and a product, and this is the former.
- **Bandwidth shaping and server-side rate limiting.** Client politeness is in; the other two mechanisms that share
  the words "rate limit" are not.
- **A multiplayer reliability layer.** [todo.md](../todo.md) records the requirements the datagram layer must not
  foreclose, and the layer itself belongs above this library when something needs it.

## The clock is a seam from the first version

Timeouts, retries, rate limits and connection races are all clock-dependent, and testing them with real sleeps is both
slow and flaky.
`cnet::clock` exists so the reactor reads time from something a test can move, and `cnet::manual_clock` is that
something.
It is here now rather than later because retrofitting one means touching every place that reads a clock.
