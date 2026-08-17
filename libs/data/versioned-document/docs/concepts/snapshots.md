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

## Seeding a filtered sweep costs the filter, not the snapshot

A snapshot is a whole document, so reading one is O(document) — which would make it worthless to the call that needs it most.
`op_builder::build` materializes the touched entities of an edit, usually one, and it does that on every keystroke.

So a filtered sweep reaches into the snapshot by entity lookup rather than walking it: `|wanted| · log(entities)`.
`materialize_stats::snapshot_entities_read` reports what it actually touched, because a result comparison cannot tell a lookup from a walk.

## A snapshot can be advanced instead of recomputed

`surviving(child)` is `surviving(parent)` with the child's assignments overwriting their paths — **on a single-parent edge, and only there**.
The child dominates every writer it overwrites, and it contributes no other, so nothing else in the set can change.

`advance_snapshot` moves the cache entry from parent to child for the cost of the child's assignments.
No `create_owning_copy`, no walk of the document, nothing that scales with the document at all.
A session that calls it on every accepted op keeps its head **permanently one op from a snapshot**, so a materialization never gets slower with the session.

Two consequences worth stating rather than discovering:

- **The entry is re-keyed, not mutated under its old key.**
  What is installed at the child genuinely is `surviving(child)`, so nothing about "an op id commits to everything behind it" is bent.
  Every `raw_document` borrowing the old entry is invalidated, exactly as one is by any cache modification.
- **Overwriting strands the old value bytes**, since the arena is append-only chunks and a view already handed out must not move.
  They are counted, and the snapshot is rebuilt from scratch once they outweigh the live ones.
  That rebuild is the only O(document) event on this path, and it happens a logarithmic number of times rather than once per op.

**It is caller-driven on purpose.**
During a wide fan — a gizmo emitting one op per frame off the same state — the frames are siblings of each other.
Advancing onto one would leave every other frame with no snapshot to terminate at and a whole history to replay.
So the snapshot stays at the branch point for the duration, and moves only when a frame is accepted as history, which only the application knows.

A merge cannot be advanced onto, because two parents means the identity above does not hold.
Deriving it there would need the parent's `superseded` set, which is the thing a snapshot deliberately does not store.
Nor can a skeleton, whose assignments are gone rather than empty.

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
