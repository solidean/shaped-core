# clean-net cheat sheet

Sockets and datagrams, HTTP and WebSocket over a backend seam, and a loopback dev server.
Namespace `cnet`; headers included by full path from `src/`.

> **The transport and the protocol clients are peers, not layers.** A browser has no sockets and does have HTTP, and
> wasm is tier 1 — so each piece reports its own availability.
> [readme.md](readme.md) is the front door; [docs/structure.md](docs/structure.md) is the roadmap and the support matrix.

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type or the intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `cnet`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../clean-core/docs/logging.md).

**Early stage.** Only the vocabulary below exists today; everything in [docs/structure.md](docs/structure.md) marked planned is not here yet.

## Everything at once

```cpp
#include <clean-net/all.hh>   // fwd.hh is the API index: every name, with the line that says what it is
```

## Failure

```cpp
#include <clean-net/common/error.hh>

cnet::error_code::unsupported;        // never on this platform — decide once at startup
cnet::error_code::backend_missing;    // not compiled into this build — changes when a dependency is fetched
cnet::error_code::timed_out;          // happened this time, worth retrying
cnet::error_code::certificate_rejected; // the chain was built and REFUSED, unlike tls_handshake_failed

cnet::error{.code = ..., .native_code = 0, .message = ...}; // copyable, so a caller can latch the first failure
e.native_code;                        // errno / WSAGetLastError / a TLS code; 0 when no platform call was made
to_string(e);                         // ADL hook — what makes erasure into cc::any_error carry the message
cnet::to_string(cnet::error_code);    // cc::string_view — for a log line, never to parse

cnet::unsupported_here("listening");                        // cnet::error
cnet::backend_missing("the curl backend", "none was found"); // cnet::error
```

`cc::result<T, cnet::error>` converts implicitly to `cc::result<T, cc::any_error>`, so a caller who does not care about the code loses nothing.

## Capability level

```cpp
#include <clean-net/common/level.hh>

cnet::http_level::fetch;       // 0 — a browser: restricted headers, no connection control, redirects hidden
cnet::http_level::client;      // 1 — every header, redirect control, pooling, streamed request body, per-phase timeouts
cnet::http_level::connection;  // 2 — the socket itself: upgrades, client certificates, socket options

cnet::to_string(cnet::http_level);  // cc::string_view
```

Ordered on purpose: code written against a level runs on every backend at that level or above.
`client` is the portable target — everything in it is expressible over a platform HTTP stack.
**The transport is not on this ladder**; sockets answer `is_supported()` instead.

## Time

```cpp
#include <clean-net/common/deadline.hh>

cnet::deadline::after_ms(30000);   // relative: the operation knows when it started, the caller does not
cnet::deadline::after_secs(1.5);
cnet::deadline::never();           // spelled out, because it is never wanted by accident
d.is_finite();                     // bool — false only for never()
```

One deadline covers a whole operation rather than each step, so resolve + connect + handshake + read share one budget.

```cpp
#include <clean-net/common/clock.hh>

cnet::system_clock();              // cnet::clock& — process-wide, never destroyed
clock.now_ns();                    // i64 monotonic; only DIFFERENCES are meaningful
cnet::manual_clock(start_ns);      // a clock that moves only when a test moves it
mc.advance_ms(5); mc.advance_ns(n); mc.set_ns(n);  // set_ns asserts it never goes backwards
```

## Addresses

```cpp
#include <clean-net/address/ip_address.hh>

cnet::ip_address();                                  // family() == none — not a place
cnet::ip_address::parse("192.168.0.1");              // cc::optional<ip_address>
cnet::ip_address::parse("2001:db8::1%3");            // `%scope` is numeric only; brackets are NOT accepted here
cnet::ip_address::any(cnet::ip_family::v4);          // 0.0.0.0 — a bind target, never a destination
cnet::ip_address::loopback(cnet::ip_family::v6);     // ::1
cnet::ip_address::from_v4(span_of_4); from_v6(span_of_16, scope);

a.family(); a.is_valid(); a.octets();                // cc::span<u8 const>, network order, 4 or 16 wide
a.scope_id();                                        // u32 — part of the address, so fe80::1%3 != fe80::1%4
a.is_unspecified(); a.is_loopback(); a.is_multicast(); a.is_link_local(); a.is_v4_mapped();
a.to_string();                                       // canonical RFC 5952: lower case, longest zero run as `::`
```

Two gotchas worth knowing.
**A leading zero in IPv4 is rejected**, because `010.0.0.1` is octal to some resolvers and decimal to others.
**`::ffff:1.2.3.4` compares unequal to `1.2.3.4`** while reaching the same machine.

```cpp
#include <clean-net/address/endpoint.hh>

cnet::endpoint::parse("127.0.0.1:8080");   // cc::optional<endpoint>
cnet::endpoint::parse("[::1]:443");        // IPv6 MUST be bracketed — that ambiguity is what brackets remove
cnet::endpoint(addr, 8080);
e.address; e.port;                          // port 0 = "any free port", which is what a test server binds to
e.is_valid(); e.to_string();                // brackets an IPv6 address back
```

A name is not an endpoint: `parse("example.com:80")` fails, because resolving needs the OS and can block.
