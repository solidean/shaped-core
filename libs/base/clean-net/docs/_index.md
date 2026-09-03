# clean-net docs

The `cnet` networking library.
Start at the [readme](../readme.md) for what it is, and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

- [structure.md](structure.md) — the layering, the per-platform support matrix, what is done and what is planned.
- [transport-seam.md](transport-seam.md) — how a connection stops being "a socket", and how a bad network becomes a unit test.
- [cancellation.md](cancellation.md) — `cnet::cancel_token`, and why it is not the deadline.
- [name-resolution.md](name-resolution.md) — the blocking lookup on a worker, its cache, and the race above it.
- [tls.md](tls.md) — Mbed TLS as a wrapper over any connection, and the trust store that is the harder half.
- [websockets.md](websockets.md) — messages over a stream, what the layer answers for you, and the handshake that stops being HTTP.
- [http.md](http.md) — the capability ladder, bodies and backpressure, pooling, and why retries live with the rate limit.

Two runnable examples, which are the fastest way to see how the API reads:

- `uv run dev.py example clean-net/download` — the HTTP client: a buffered body, a streamed one, a redirect, a
  failure, and politeness.
- `uv run dev.py example clean-net/dev-server` — routes, static files, a streamed response and a WebSocket.

Both play **both ends over loopback**, so neither needs the internet: an example that fails because a remote host
moved teaches nobody anything.

Repo-wide context worth having beside these:

- [docs/platforms.md](../../../../docs/platforms.md) — the support tiers, and why wasm being tier 1 shapes this library.
- [logging](../../clean-core/docs/logging.md) — every diagnostic here goes through `cc::rec`, on the `cnet` domain.
