# cnet: the loopback dev server, and WebSocket

**Status:** decided, not built.

## What it is for

An in-browser debug UI: our process serves a small page and streams live data to it.
The natural consumer is `cc::rec` — the recording stream that logging, profiling, stats and tracing all write into —
and a live view of it in a browser is the obvious front end beside the in-process ImGui one.

That use case is also why the WebSocket and chunked-response work is worth doing at all: a debug view of a recording
stream is unbounded in length.

## What separates this from a web server

A large amount of work that is entirely about hostility: slowloris and other slow-read attacks, request smuggling
between a proxy and a backend, connection exhaustion, path traversal in static file serving, compression bombs, and a
threat model where every byte is attacker-controlled.

A server bound to `127.0.0.1` faces none of it, because the only thing that can reach it is a process already running
as the same user.
A server bound to `0.0.0.0` faces all of it — and **the difference between the two is one line of configuration that
somebody will change**.

## The decisions

**A loopback dev server: routes, static files, WebSocket, chunked responses, finite limits, no TLS.**

**No server-side TLS**, and the reason is worth keeping rather than rediscovering: browsers treat `http://localhost`
as a secure context, so a local debug UI needs no certificate, no self-signed trust prompt and no server-side TLS code
at all.
That removes the single largest chunk of server work.

Server-side TLS may be added later and would still be testing- and development-only.
If the scope ever genuinely extends to hostile input, that is a **separate library**, not a bigger version of this
one.

**What it refuses to grow into:** virtual hosts, TLS termination, HTTP/2, proxying, authentication frameworks, a
plugin architecture.
Each arrives as a small reasonable request and together they are a web server.

**Binding beyond loopback is a named boolean, off by default, with a `cc::rec` warning when set.**
A distinct field rather than a bind-address string that happens to say `0.0.0.0`, because the point is that the
choice is visible.

**The docs say plainly that it is not hardened for hostile input** — rather than implying safety by listing
mitigations.

## The limits, which exist anyway

A dev server will eventually be exposed by someone with an SSH tunnel or a container port mapping.
These cost very little and are the difference between "unsuitable for hostile input" and "trivially crashable":

- A maximum request line and header block size, and a maximum header count.
- A read timeout on the header phase, which is what makes slow-read attacks uninteresting.
- A maximum concurrent connection count, with a bounded accept backlog.
- A maximum request body size, finite by default.
- Static file serving that **resolves and confines paths under its root** rather than concatenating them.

They are not a claim that the server is safe to expose.

## The shape

```cpp
struct cnet::http_server_description
{
    i32 port = 0;   // 0 = pick a free one

    /// Binding beyond loopback is a deliberate, named act.
    /// This server is not hardened for hostile input; see the library docs.
    /// Setting this logs a warning through cc::rec at startup.
    bool bind_all_interfaces = false;

    isize max_header_bytes = 16 * 1024;
    isize max_body_bytes = 8 * 1024 * 1024;
    i32 max_connections = 64;
    i32 header_timeout_ms = 5000;
};

class cnet::http_server
{
public:
    /// Fails with `unsupported` on wasm, where a process cannot listen.
    [[nodiscard]] static cc::result<cc::unique_ptr<http_server>, error> try_create(http_server_description const& desc = {});

    void route(cc::string_view method, cc::string_view pattern, route_handler handler);

    /// Serves files under `root`, confined to it: a resolved path outside `root` is a 404, never a read.
    void serve_directory(cc::string_view url_prefix, cc::string_view root);

    void websocket(cc::string_view pattern, websocket_handler handler);
};
```

## The first example

**A minimal routes-and-static-files example first**, with the `cc::rec` live view later.

That was the maintainer's call over the alternative of leading with the recording view.
The recording view is the use case that *justifies* the streaming work, but it is not the example that teaches
someone what the server is.

## WebSocket

Client and server framing, over both the native transport and the browser backend.

The browser side is not ours to write — a `WebSocket` in a browser is the platform's — so this has the same backend
shape as HTTP, and for the same reason.

## Depends on

- [cnet-http-client.md](cnet-http-client.md) for the HTTP/1.1 parser, which the server side shares.
- [cancellation](../../libs/base/clean-net/docs/cancellation.md) for shutdown, which exists now: a listener parked on
  an accept stops through its token rather than waiting for a deadline that was deliberately never set.
