# Pending work

What is **agreed but not built**, one document per feature.

Three places hold the state of a piece of work, and they do not overlap:

- **Commits** carry what was built, and why each decision inside it went the way it did.
- **A library's own `docs/`** carries the design of what exists — the shape a reader needs to use it.
- **This folder** carries what is decided and not yet written.

The point of the split is that a session can end anywhere without losing the argument.
A decision reached in conversation and then not written down is a decision that will be made again, differently.

## What belongs here

A document per feature, not per task.
Each one says what the feature is, what was settled and *why*, what is still open, and which traps are already known.

Write it so a reader who was not in the conversation can act on it.
That means the reasoning travels with the decision: "use a token bucket" is a note, "a retry policy without a rate
limit is how a transient failure becomes a self-inflicted denial of service" is why anyone would.

A document leaves this folder when the work lands.
Its reasoning moves into the library's own docs, and the record of the change is the commit.

## clean-net (`cnet`)

The networking library, designed in a two-round review and built as far as TCP.
Its [readme](../../libs/base/clean-net/readme.md) is the front door and its
[structure.md](../../libs/base/clean-net/docs/structure.md) has the support matrix and what is done.

In the order it is meant to be built:

- [cnet-http-client.md](cnet-http-client.md) — the capability ladder, HTTP/1.1, bodies and politeness.
- [cnet-http-backends.md](cnet-http-backends.md) — the browser, a system libcurl, and the order they are chosen in.
- [cnet-dev-server.md](cnet-dev-server.md) — a loopback server and WebSocket, and what they refuse to become.
- [cnet-datagrams.md](cnet-datagrams.md) — UDP, and the requirements a future multiplayer layer must not have lost.
- [cnet-reactor-scaling.md](cnet-reactor-scaling.md) — IOCP and epoll, and what would make them worth it.
- [cnet-housekeeping.md](cnet-housekeeping.md) — the small things, including one real defect in babel's glTF reader.
