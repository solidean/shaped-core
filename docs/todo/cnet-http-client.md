# cnet: the HTTP client

**Status:** decided, not built.
The capability ladder already exists in `cnet::http_level`; nothing behind it does.

## The layering, restated because everything here depends on it

**HTTP is an interface with backends, and the transport is one of them — not the layer underneath.**

A browser cannot open a socket, cannot listen and cannot send a datagram, and it *does* have `fetch`.
Since wasm is tier 1, an HTTP client written over our own TCP could not exist on a platform we support.

The consequence for this API: **the shared surface stays within what a browser `fetch` can express**.
Anything more reaches for the transport directly and is unavailable on wasm.

## The capability ladder

`cnet::http_level` is built and documented.
What is missing is everything that reads it.

- `fetch` (0) — a browser: method, URL, a restricted header set, a body.
  Redirects are followed for you and cannot be inspected, connections are not yours to see, CORS applies.
- `client` (1) — every header, explicit redirect control, connection reuse and pooling, a streamed request body,
  response trailers, a timeout per phase.
  **The portable target**, because everything in it is expressible over a platform HTTP stack.
- `connection` (2) — the socket itself: protocol upgrades, client certificates, per-socket options.
  Native only, and separate precisely *because* taking the socket is not expressible over a platform stack.

A call needing a level the backend lacks **fails loudly** — a `cc::result` error, or an assert for a programming
error.
Never a silent degrade: a dropped header is discovered in production, a refused call in the first test run.

## Bodies

**The push sink with backpressure is the primitive.**
Buffered and to-file are wrappers over it.

```cpp
namespace cnet
{
/// A sink returns how many bytes it consumed.
/// Returning less than the chunk applies backpressure: the transport stops reading, and TCP's own window does the rest.
using body_sink = cc::function_ref<isize(cc::span<byte const> chunk)>;

struct request_options
{
    /// Bytes buffered for this response before the transport stops reading.
    isize max_buffered_bytes = 1 << 20;

    /// Refuse a body larger than this. Platform-dependent -- see below.
    i64 max_body_bytes = impl::default_max_body_bytes();
};

[[nodiscard]] cc::shared_async<response_head> send_streaming(request r, body_sink sink);
[[nodiscard]] cc::shared_async<response> send(request r);                                  // buffered, over the above
[[nodiscard]] cc::shared_async<cc::unit> download_to_file(cc::string_view url, cc::string_view path);
}
```

**Backpressure is in the sink signature from the first version.**
It is the only part of this that is expensive to add afterwards, and an unbounded buffer on an unknown-size download
is a real failure rather than a theoretical one.

**No `cc::read_stream` or async-stream view yet.**
That waits for the async-stream design, which is itself still open.
The sink is what ships; the type-shaped view is what waits.

**The chunk callback runs on the reactor thread.**
State it in the API doc rather than in a guide: do no work there, hand the bytes on.

### The body cap is platform-dependent, and that is not symmetry

Around **3 GiB with 64-bit pointers** — the threshold where someone should stop and question their pipeline.

On **wasm32, a few hundred MiB**.
A wasm32 module's entire linear memory is capped at 4 GiB and browsers fail well below that; a mobile browser is
stricter still.
The point of the cap is that exceeding it is a *clean error a caller can handle*, and a number the platform cannot
honour delivers an out-of-memory abort instead.

The concept is constant, the number is not.
`docs/platforms.md` already carries this shape for `CC_HAS_64BIT_POINTERS`.

Log a refused-for-size failure through `cc::rec`, or a caller who hits it on wasm and not on desktop reads it as a
network error.

## Politeness — the only rate limiting in scope

"Rate limiting" names three unrelated mechanisms.
Only the first is in scope:

- **Politeness** — not hammering someone else's server, per remote host, at request admission.
- **Shaping** — not saturating a link, per transfer, at the socket read/write level; *not in scope*.
- **Protection** — refusing work that arrives too fast, per inbound peer, server-side; *not in scope*.

```cpp
struct host_policy
{
    f32 requests_per_second = 0;   // token bucket over the reactor clock; 0 = unlimited
    i32 burst = 8;
    i32 max_concurrent_requests = 6;   // usually what actually protects a remote server

    bool honor_retry_after = true;     // a 429 means wait, not retry harder

    /// Retry travels WITH the rate limit: a retry policy without one is how a transient failure becomes a
    /// self-inflicted denial of service.
    i32 max_retries = 3;
    f32 backoff_base_ms = 250;
    f32 backoff_jitter = 0.3f;
};
```

Retries are **on by default for idempotent methods only**, with jittered backoff and a small cap, and **configurable
per request**.

**No singleflight.**
Collapsing two concurrent identical requests into one requires assuming idempotence, which an HTTP client may not do
on its own.
If it is wanted it is a deliberate, opt-in thing later.

The byte path stays interceptable at the socket level so a shaper could be added without moving where reads happen —
which falls out of the push-with-backpressure design for free.

## HTTP/1.1 only in our own backend

HPACK, stream multiplexing and flow control are a different order of magnitude, and QUIC is not something to write.
For downloading files from servers we do not control, HTTP/1.1 is universally accepted; the cost of not having
HTTP/2 is throughput on many-small-requests workloads, which is not a use case here.

**Wanting HTTP/2 is a reason to select a system backend** ([cnet-http-backends.md](cnet-http-backends.md)), not a
reason to write one.

Say so in the library docs, so a throughput surprise is a documented limit rather than a bug report.

## What has to be written

Request and response types, the level checks, chunked transfer, keep-alive and pooling, redirect handling with a cap,
per-phase timeouts, and the parser.

The parser is the one component here that handles bytes from outside the process, so it is the one that earns a
fuzzer — nexus already has an API-sequence fuzzer, and this is its shape.
Agreed as a follow-up rather than a blocker.
