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

**Early stage.** The vocabulary, the reactor, TCP and the transport seam exist today; everything in [docs/structure.md](docs/structure.md) marked planned is not here yet.

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

## The reactor

```cpp
#include <clean-net/io/io_system.hh>

cnet::io_system::try_create();                          // cc::result<cc::unique_ptr<io_system>, error>; succeeds even with no sockets
cnet::io_system::try_create({.unthreaded = true, .time_source = &clk, .max_wait_ms = 50});
cnet::io_system::create(desc);                          // throwing counterpart
io->has_reactor_thread();                               // false on a threads-off build, whatever the description said
io->time_source();                                      // cnet::clock& — what deadlines are measured against
io->pending_count();                                    // a snapshot, readable from any thread
```

**There is no `poll()`.**
With threads the io_system owns one; without them it registers with clean-core's pump, so `cc::thread_pump_all()` drives it along with everything else.
A `cnet`-specific pump would be the deadlock that registry exists to prevent.

The reactor runs two things that are not sockets, which is why an io_system exists on wasm too.
A **timer** completes successfully at its deadline — a backoff, a head start, a simulated link's latency.
A **manual** operation completes when its owner signals it, and times out if nobody does.

## TCP

```cpp
#include <clean-net/transport/stream.hh>

cnet::tcp_connect(io, where);                           // cc::shared_async<cc::shared_ptr<stream_connection>>
cnet::tcp_connect(io, where, cnet::deadline::after_secs(10), {.no_delay = true, .v6_only = true});

cnet::stream_listener::try_create(io, endpoint(addr, 0));  // port 0 = pick one; local() says which
listener->accept();                                     // cc::shared_async<cc::shared_ptr<stream_connection>>, no deadline by default
listener->local();                                      // endpoint

conn->receive(buffer);                                  // cc::shared_async<isize> — the FIRST bytes, not a full buffer
conn->send(bytes);                                      // cc::shared_async<cc::unit> — completes only when all of them are gone
conn->shutdown_send();                                  // cc::result<cc::unit, error> — half-close: "that was the whole request"
conn->local(); conn->peer(); conn->is_open(); conn->close();
```

Three things worth knowing.
**A receive completes on the first bytes that arrive**, so a four-byte write answers a one-kilobyte read with four bytes — a stream has no message boundaries.
**A peer that closed fails with `connection_closed`** rather than reporting zero bytes, which would be indistinguishable from a read that has not happened yet.
**`close()` drops a reference rather than closing the handle**, because an operation the reactor still watches holds one too.
A handle closed under the reactor can be reissued to the next socket the process opens.

`bytes` passed to `send` must stay alive and unmodified until the operation completes.

## Names

```cpp
#include <clean-net/address/resolver.hh>

auto r = cnet::resolver::create(io);                     // cc::unique_ptr<resolver>; try_create reports unsupported on wasm
auto r = cnet::resolver::create(io, {.cache_ttl_ms = 60'000, .lookup = my_table});  // lookup replaces the OS, for a test
cnet::resolver::is_supported();                          // false on wasm, where the browser resolves inside fetch

r->resolve("example.com");                               // cc::shared_async<cc::vector<ip_address>>
r->resolve(host, {.family = cnet::address_family_preference::v6_only, .timeout = cnet::deadline::after_secs(5)}, token);
r->clear_cache(); r->cached_host_count();
```

Four things worth knowing.
**A literal address resolves to itself** — no worker, no cache, no failure — so a caller need not know which kind of string it holds.
**The whole answer is cached whatever the caller asked for**, so a v4-only caller warms the cache for a v6-only one; a failure is never cached.
**The timeout bounds the wait, not the work**: `getaddrinfo` cannot be aborted, so a resolve that times out still occupies its worker until the OS returns.
**A resolve is the one operation that can stall a threads-off process**, because the lookup then runs inside `cc::thread_pump_all()`.
That is accepted rather than mechanised away: such builds are for debugging, and wasm — the configuration that ships single-threaded — never resolves at all.

## Connecting to a name

```cpp
#include <clean-net/transport/connect.hh>

cnet::connect_to_host(io, *resolver, "example.com", 443);          // cc::shared_async<cc::shared_ptr<stream_connection>>
cnet::connect_to_host(transport, *resolver, host, port, {.timeout = cnet::deadline::after_secs(10),
                                                         .attempt_delay_ms = 250}, token);
```

Resolve and connect as one operation, racing the addresses (RFC 8305) — IPv6 first, then one of each family, staggered by the attempt delay.
**One budget covers the whole thing**, resolve included, because a per-step timeout lets a four-address host take four times what the caller asked for.
**Every attempt gets a child of your token**, so cancelling yours cancels the race while the race cancels only its losers.
If every attempt fails you get the FIRST failure, which is the one about the address the OS thought best.

## TLS

```cpp
#include <clean-net/tls/tls.hh>

cnet::tls_is_supported();                               // false on wasm, where the browser holds the TLS

cnet::tls_connect(connection, "example.com");           // cc::shared_async<cc::shared_ptr<stream_connection>>
cnet::tls_connect(connection, host, {.trust = {.additional_roots_pem = {my_ca}}, .alpn = {"http/1.1"}}, d, token);
cnet::tls_accept(connection, {.identity = id, .alpn = {"http/1.1"}});   // the server side
cnet::tls_negotiated_alpn(*conn);                       // cc::string_view, empty if none was agreed

cnet::tls_make_self_signed("localhost");                // cc::result<tls_identity, error> — a dev server's certificate
```

**TLS is a wrapper, not a transport**: it takes a connection and hands back a connection, so it composes over the real network, the virtual one, and a simulated bad link alike.
**`hostname` is the NAME, not the address** — it is what the certificate must match and what goes out as SNI, so passing an address is how verification gets accidentally disabled.
**The trust default is this machine's store**, and a platform whose adapter is not written yet reports `unsupported` rather than pretending to have no roots.
**`allow_any_certificate` is settable from code only**, never from configuration.
[docs/tls.md](docs/tls.md) has the trust story, which is the harder half.

## Cancelling

```cpp
#include <clean-net/common/cancel.hh>

auto const token = cnet::cancel_token::create();   // the default cancel_token() allocates nothing and never cancels
cnet::tcp_connect(io, where, cnet::deadline::after_secs(10), {}, token);
conn->receive(buffer, cnet::deadline::after_secs(30), token);
listener->accept(cnet::deadline::never(), token);

token.cancel();          // every operation it was given to ends as cancelled, now or later
token.is_cancelled();    // a token stays cancelled: a later call fails at once rather than starting

auto const child = token.create_child();   // cancelled by its parent, and cancellable on its own — how a token composes downward
```

**A token groups where a deadline bounds** — one token per request, one deadline per step, which is why they are two parameters.
**A cancelled outcome is `cc::async_error::is_cancelled()`**, not an error carrying a code, so `handle->try_error()->is_cancelled()` is the branch.
**Cancelling ends the operation, not the connection**: the socket stays open and usable.
[docs/cancellation.md](docs/cancellation.md) has the race and why it is not one.

## Standing something else in for the network

```cpp
#include <clean-net/transport/backend.hh>       // the seam: connection_backend, listener_backend, transport
#include <clean-net/transport/virtual_transport.hh>
#include <clean-net/transport/simulated_transport.hh>

auto net = cnet::virtual_network(io);                   // an in-process network: no socket, no port, no loopback
auto link = cnet::simulated_transport(io, net, {.latency_ms = 50, .reset_after_bytes = 4096, .seed = 7});

cnet::stream_listener::try_create(link, where);            // the transport overloads of the same factories
cnet::tcp_connect(link, where);                         // ...so the code under test never changes
```

`cnet::link_conditions` — `latency_ms`, `jitter_ms`, `loss_probability`, `bandwidth_bytes_per_sec`, `reset_after_bytes`, `seed`.
`duplicate_probability` and `reorder_probability` are datagram-only and ignored on a stream.

Three things worth knowing.
**Every condition is off by default**, so a simulated link nobody configured is indistinguishable from the one underneath — without that, a passing test proves nothing.
**Delays are paid on the io_system's clock**, so a `manual_clock` turns a 200 ms link into microseconds.
**The seed is logged when the link is built**, which is what makes a failing run replay from two numbers.

`native_transport(io)` is the real one, and what `tcp_connect(io, ...)` uses; it is cheap enough to construct at the call site.
[docs/transport-seam.md](docs/transport-seam.md) is the design.