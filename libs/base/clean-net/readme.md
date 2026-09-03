# clean-net — networking, namespace `cnet`

Sockets and datagrams, HTTP and WebSocket over a backend seam, name resolution, and a loopback dev server.
Depends on clean-core and nothing else.

Early stage — [docs/structure.md](docs/structure.md) says what exists today and what is planned.

```cpp
#include <clean-net/all.hh>

// an address is a value with no OS type in it
auto const local = cnet::endpoint::parse("[::1]:8080").value();

// every operation that touches the network carries a finite deadline
auto const budget = cnet::deadline::after_secs(30);
```

## The one thing to know first

**The transport and the protocol clients are peers, not layers.**

The textbook stack is a tower — sockets, then TLS, then HTTP.
A browser breaks it: JavaScript cannot open a TCP connection, cannot listen on a port and cannot send a datagram, but
it *does* have `fetch` and `WebSocket`.
The bottom of the tower is missing and the top is provided, and WebAssembly is a tier-1 platform here
([docs/platforms.md](../../../docs/platforms.md)).

So HTTP and WebSocket are interfaces with backends, one of which is written over our own transport and one of which is
the platform's own stack.
Three pieces report their availability separately:

| | wasm | Windows / Linux / macOS | iOS / Android |
|---|---|---|---|
| TCP / UDP sockets | absent | present | present |
| HTTP and WebSocket clients | present, `fetch` backend | present, native backend | present, either |
| listening server | absent | present | present |

The consequence for the API is that **the shared HTTP surface stays within what a browser `fetch` can express**.
Anything more reaches for the transport directly and is unavailable on wasm, and `cnet::http_level` is how a caller
asks which it has.

## What is not here, on purpose

- **The server is not hardened for hostile input.** It is a loopback dev server for an in-browser debug UI.
  Binding it beyond loopback is a named, logged act, and it is still not a web server.
- **HTTP/1.1 only** in our own backend.
  HTTP/2 is a reason to reach for a system backend, not something we implement.
- **No blocking helpers.** Nothing here requires blocking to obtain a result, because a browser main thread cannot
  block and a render thread should not.

## Include convention

`#include <clean-net/all.hh>` for everything, or the individual headers under `clean-net/` for less.
`fwd.hh` is the API index: every name this library exposes is declared there with the one line that says what it is.

## Elsewhere

- [cheat-sheet.md](cheat-sheet.md) — the API at a glance.
- [docs/structure.md](docs/structure.md) — the roadmap and the per-platform support matrix.
- [todo.md](todo.md) — what is agreed but not built.
