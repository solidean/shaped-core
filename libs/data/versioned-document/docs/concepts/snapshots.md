# Concept: snapshots

Materializing a head means replaying everything reachable from it, so a naive materialization is linear in history.
A document edited for years would get slower every day it is used, which is the difference between a format that lasts and one that does not.

A **snapshot** is a materialization cached against the op it was computed at.
Materializing walks back until it meets one, and stops there.
Real editing produces mostly-linear histories, so this turns "replay everything since the document was created" into "replay the last few ops".

**A snapshot must be indistinguishable from the replay it replaces.**
That is the property everything else here rests on, and it is checked exhaustively against a brute-force oracle over a generated corpus of DAG shapes.

## A snapshot is `surviving` only

A cached snapshot is exactly a `raw_document` — the surviving writers and nothing else — over bytes it owns, because an op whose payload was pruned away cannot be borrowed from.

Storing the `superseded` sets alongside was designed first and **rejected on size**.
They total `32 B × (all historical assignments − distinct paths)`, so a snapshot would grow with the very history it exists to replace.
The asymptotics are wrong, not merely the constant.
[decisions.md](../decisions.md#a-snapshot-stores-surviving-only-and-its-validity-is-decided-at-use) carries the numbers.

## Validity is decided at use, not at creation

`surviving(X)` is a function of X's own causal past alone.
State flows forward through the sweep, so the state at X never depends on anything outside X's ancestry.
**So any op may be snapshotted, from any sweep, and there is no eligibility question at creation time.**

Whether a snapshot may be *used* is a different question, and it is re-checked against today's DAG on every sweep:

1. Walk back from the heads, treating any op the cache holds as a terminator whose parents are not expanded.
2. The walked set is then parent-closed except at terminators, so its sources are the in-degree-0 set the topological sort already computes.
3. **Seed a snapshot only where there is exactly one source and it is that snapshot.**

That single source is an ancestor of everything walked, and an ancestor cannot present a stale branch.
A second condition is easy to miss and is enforced too: no op other than the seed may have a parent that is in the graph but outside the walk, or it would replay from a partial ancestry.

Everything else falls back to a plain replay, which costs time and never a result.
**Correctness never depends on the optimism; only speed does.**

The rule that had to be recorded rather than merely fixed: *"every source is cached"* is **unsound**.
Materializing `{T, X}` with X a distant ancestor of T gives two cached sources, and unions their surviving sets into a multi-value nobody wrote.

## The cache is derived, and dropping it is invisible

The in-memory cache holds whole materializations, so it is bounded and evicts least-recently-used.
Dropping any of it at any moment changes results not at all — only how long the next sweep takes.

Nothing installs a snapshot behind a caller's back.
The cache is passed in explicitly, so materializing is not a hidden mutation reached through a const graph.

Adding ops never invalidates an entry either, and there is deliberately no hook that says otherwise: an op id commits to everything behind it, so `surviving(id)` cannot change once computed.

## Persisted, and the one kind that is not derived

A file may store a snapshot so a load need not replay history at all.
Two kinds, and the distinction has to be visible everywhere one is touched:

- **droppable** (`required = 0`) — a cache like any other.
  Deleting the row leaves the document materializable, just more slowly, and one that will not decode is a load issue naming its op.
- **required** (`required = 1`) — **load-bearing**.
  The history behind this op is gone, so deleting the row destroys data, and one that will not decode fails the open outright.

A required snapshot is **pinned** in the cache, so a tool shedding cache memory cannot reach it.

Persisting is **explicit**, and never a side effect of publishing.
A snapshot is derived, so writing one on a heuristic would make publishing non-idempotent and grow the file with caches nobody asked for.

Only [pruning](pruning-and-recovery.md) creates a required snapshot, and only [recovery](pruning-and-recovery.md#recovery-from-an-untrusted-peer) turns one back into a droppable one.
[format.md](../../../versioned-document-file/docs/format.md#snapshots--materialization-caches) specifies the bytes.
