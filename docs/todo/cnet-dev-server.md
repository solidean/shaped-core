# cnet: what is still missing from the dev server

**Status:** the server, WebSockets, static files, streamed responses and the first example are built.
The browser backend is not.

The server's own design lives in [clean-net/docs/http.md](../../libs/base/clean-net/docs/http.md#the-server), and
WebSockets in [clean-net/docs/websockets.md](../../libs/base/clean-net/docs/websockets.md).
What is kept here is the part that has not been built, plus the decisions that would otherwise be rediscovered.

## What it is for

An in-browser debug UI: our process serves a small page and streams live data to it.
The natural consumer is `cc::rec` — the recording stream that logging, profiling, stats and tracing all write into —
and a live view of it in a browser is the obvious front end beside the in-process ImGui one.

## Still to build

**The browser backend**, where `WebSocket` is the platform's and not ours to write.
Same backend shape as HTTP, and for the same reason.

**The `cc::rec` live view**, over the streaming and WebSocket routes that now exist.
`uv run dev.py example clean-net/dev-server` is the routes-and-static-files example that comes first — that was the
maintainer's call over leading with the recording view, which is the use case that *justifies* the streaming work but
is not the example that teaches someone what the server is.

**A symlink out of a served root**, and streaming a large static file rather than reading it whole.
Both want a path surface clean-core deliberately does not have; see
[structure.md](../../libs/base/clean-net/docs/structure.md#what-clean-core-is-missing-that-this-library-wants).
That surface is **its own change, and a virtualized one** -- the same seam the transport has, so a test stands a
filesystem in the place of the real one.

## The decisions worth keeping

**No server-side TLS.**
Browsers treat `http://localhost` as a secure context, so a local debug UI needs no certificate, no self-signed trust
prompt and no server-side TLS code at all.
That removes the single largest chunk of server work.
Server-side TLS may be added later and would still be development-only.

**What it refuses to grow into:** virtual hosts, TLS termination, HTTP/2, proxying, authentication frameworks, a
plugin architecture.
Each arrives as a small reasonable request and together they are a web server — which would be a **separate library**,
not a bigger version of this one.

**The docs say plainly that it is not hardened for hostile input**, rather than implying safety by listing
mitigations.
A server bound to `127.0.0.1` faces none of that threat model, because the only thing that can reach it is a process
already running as the same user.
A server bound to `0.0.0.0` faces all of it, and the difference between the two is one line of configuration that
somebody will change — which is why it is a named boolean that logs a warning.
