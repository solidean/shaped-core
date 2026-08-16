# blob-cache docs

A persistent, multi-process cache for expensive derived bytes.
Namespace `bcache`.

Start at the [readme](../readme.md) for what it is, and the [cheat sheet](../cheat-sheet.md) for the whole API.

## Topics

* [design.md](design.md) — the architecture and the reasoning: the disposability invariant, the actor that owns the
  connection, singleflight, the data model, the eviction policy, and what each of them rules out.

The repo-wide conventions this library follows are [docs/coding-guidelines.md](../../../../docs/coding-guidelines.md).
