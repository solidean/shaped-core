# The HTTP client

What is here, what is deliberately not, and where each decision came from.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## The layering, restated because everything here depends on it

**HTTP is an interface with backends, and the transport is one of them — not the layer underneath.**

A browser cannot open a socket, cannot listen and cannot send a datagram, and it *does* have `fetch`.
Since wasm is tier 1, an HTTP client written over our own TCP could not exist on a platform this repo supports.

The consequence for the API: the shared surface stays within what a browser `fetch` can express.
Anything more reaches for the transport directly, and `cnet::http_level` is how a caller asks which they have.

## The capability ladder

- **`fetch` (0)** — a browser: method, URL, a restricted header set, a body.
  Redirects are followed for you and cannot be inspected, connections are not yours to see, CORS applies.
- **`client` (1)** — every header, explicit redirect control, connection reuse and pooling, a streamed request body,
  response trailers, a timeout per phase.
  **The portable target**, because everything in it is expressible over a platform HTTP stack.
- **`connection` (2)** — the socket itself: protocol upgrades, client certificates, per-socket options.
  Native only, and separate precisely *because* taking the socket is not expressible over a platform stack.

`cnet::native_http_client` reports `client`.
The socket underneath is real, and nothing hands it to a caller yet.

A call needing a level the backend lacks fails loudly rather than degrading: a dropped header is discovered in
production, a refused call in the first test run.

## Bodies, and why backpressure is in the signature

`body_sink` returns how many bytes it consumed.
Returning less than the chunk stops the transport reading more, and TCP's own window does the rest.

**That is the primitive, and buffering is written over it.**
`http_send` is the sink that keeps the bytes and nothing else.
Backpressure is the one part of this that cannot be added afterwards, and an unbounded buffer on a download of
unknown size is a real failure rather than a theoretical one.

The sink **runs on the reactor thread**: hand the bytes on, do no work there.

It owns its callable, unlike the parser's, because a request outlives the call that started it — a reference to a
lambda written at the call site would dangle before the first byte arrived.

### The body cap is platform-dependent, and that is not asymmetry

About **3 GiB with 64-bit pointers** — the threshold where someone should stop and question their pipeline.
On **wasm32, a few hundred MiB**: the whole linear memory is capped at 4 GiB and browsers fail well below that.

The concept is constant and the number is not.
The point of a cap is that exceeding it is a *clean error a caller can handle*, where a number the platform cannot
honour delivers an out-of-memory abort instead.

## Connections are pooled

Keyed on origin, so a second request to the same server pays no connect and no handshake.

**Reuse is speculative by nature.**
A server may close an idle connection at any time and nothing tells the client until a write fails or a read returns
nothing.
So a request that fails on a pooled connection *before any response byte arrived* is retried once on a fresh one, and
that retry is what makes pooling safe rather than the bookkeeping in the pool.

A connection goes back only when the message ended cleanly: the parser said the connection could be reused, and no
bytes were left over.
Bytes left over mean the server said something this client did not ask for, and a stream nobody understands is not
one to hand to the next request.

## Politeness, and why retries live with it

"Rate limiting" names three unrelated mechanisms, and only the first is in scope:

- **Politeness** — not overwhelming someone else's server, per remote host, at request admission.
- **Shaping** — not saturating a link, per transfer, at the socket; *not in scope*.
- **Protection** — refusing work that arrives too fast, per inbound peer, server-side; *not in scope*.

`cnet::polite_http_client` is a decorator over any other client, because this is policy rather than protocol: the
same rules apply to a request that goes out over a browser's `fetch`.

**Retries travel with the rate limit, and that is why they are the same object.**
A retry policy without one is how a transient failure becomes a self-inflicted denial of service: the server that
failed because it was overloaded is the one that now gets three times the traffic.

A retry happens only where repeating the request means what the caller meant:

- an **idempotent** method — sending GET twice is what the caller asked for, sending POST twice is a second order;
- **no response byte delivered yet** — after that, repeating would deliver the body twice rather than instead;
- retries left, the token not cancelled, and budget remaining.

A `429` or `503` is waited out for as long as it asked, because a 429 means wait rather than retry harder.
Only the delta-seconds form of `Retry-After` is read; the HTTP-date form falls back to the ordinary backoff, since a
client that guesses at a date it cannot parse waits for the wrong length of time.

The backoff is jittered.
Without that, a hundred clients that failed together retry together, which is the herd the backoff was there to
prevent.

**The deadline covers the queueing.**
A request that spends its whole budget waiting for a token fails rather than being sent late to a server that stopped
waiting for it.

**No singleflight.**
Collapsing two concurrent identical requests into one requires assuming idempotence, which an HTTP client may not do
on its own.

## HTTP/1.1 only, in our own backend

HPACK, stream multiplexing and flow control are a different order of magnitude, and QUIC is not something to write.
For downloading files from servers we do not control, HTTP/1.1 is universally accepted; the cost of not having HTTP/2
is throughput on many-small-requests workloads, which is not what this library is for.

**Wanting HTTP/2 is a reason to select a system backend, not a reason to write one.**

## What the parser refuses, and why it refuses rather than repairs

Almost every HTTP attack that is not an application bug is a disagreement about framing: two parties reading the same
bytes as different numbers of messages.

So the parser refuses bare LF line endings, a space before a header colon, an obs-fold continuation, more than one
`Content-Length`, and a message carrying both `Content-Length` and `Transfer-Encoding`.

The cost is a response from somebody's nonconforming server that we will not read.
The alternative is being the party that reads it differently from everyone else.

The serializer is the matching boundary on the way out, and the only one: a newline in a header name or value would
end the head and start something the caller did not write, so every byte is checked there rather than wherever the
header was set.

## Still to come

Request bodies that stream rather than arriving as one span, response trailers reaching the caller, and the parser's
fuzzer — it is the one component here that handles bytes from outside the process, so it is the one that earns one.
