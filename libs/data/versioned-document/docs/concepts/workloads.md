# Concept: the workload this is shaped for

[docs/philosophy.md](../../../../../docs/philosophy.md#optimize-for-the-workload-we-actually-have) asks a design to name its primary workload.
This is vdoc's, and several decisions elsewhere point here rather than restating it.

Everything below is about **what is common**, never about what is allowed.
The DAG supports arbitrary branching, arbitrary merges and arbitrary conflict, and none of that is second-class in correctness terms.
It is second-class in *speed* terms, deliberately, and each place that trade is taken says so and says what it falls back to.

## The three shapes

**History is mostly linear.**
One person editing one document produces a chain.
Branches, merges and genuine conflicts are real and supported, and they are rare.
Long-running branches that diverge for a while and merge back are rarer still.

**The common op is small.**
It touches **at most about a hundred entities, and usually exactly one** — a property changed, an object moved, a component added.
An op that rewrites the document is a real thing (an import, a bulk operation) and is not what the fast paths are tuned for.

## Continuous editing goes wide

While a gizmo is being dragged, the editor emits **one op per frame, each branching from the same state S** — not a chain, a fan.
Only the last one becomes history.

That shape is chosen so that the undo stack gets one entry for the whole drag rather than six hundred, and so that a half-finished drag is never a thing anyone else can see.
Two consequences run through the library:

- **The intermediate frames live in the in-memory op graph and are never written to a file by default.**
  Publishing is explicit, so this needs no mechanism — but it does mean the graph accumulates ops nothing will ever descend from.
  [`op_graph::drop_leaf`](../../src/versioned-document/op_graph.hh) is how a session forgets them, and `leaves()` is how it finds them without having tracked them.
- **A snapshot must stay at the branch point for the duration of the drag.**
  The frames are siblings of each other, so moving the snapshot onto one would leave every later frame with no snapshot to terminate at.
  This is why [`advance_snapshot`](snapshots.md#a-snapshot-can-be-advanced-instead-of-recomputed) is caller-driven rather than automatic.
  Only the application knows when a frame has been accepted as history.

Materializing a frame is unaffected by how wide the fan is, because a sweep walks parent edges and never child ones.
At the raw layer the fan costs memory, and nothing else.

### At the typed layer, chain the frames instead of fanning them

**This is the one place the fan is expensive, and the fix does not change anything a user sees.**

[`vdoc::apply`](interpretation.md#applying-an-op-incrementally) evolves a document from one op to a descendant.
Frame *k+1* is not a descendant of frame *k* — they are siblings — so the fast path cannot apply, and every frame of a fanned drag costs a **full re-parse** of the whole document.

Making each frame a single-parent child of the previous one fixes it completely, and the user-visible behaviour is identical.
The intermediates are still never published, the undo stack still gets one entry, and on release the chain is dropped with `drop_leaf` and one op is built from the branch point.
What changes is that every frame is now the shape the snapshot and the apply are both built for.

Measured at 8,000 entities, per frame:

| drag shape | per-frame cost |
|---|---|
| fanned off one parent | 19.9 ms |
| chained, dropped on release | 0.009 ms |

The [benchmark](../benchmarks/edit-latency-benchmark.md) measures both, so the gap cannot quietly close or quietly widen.

## The number to design against

**A ≤100-entity linear op, end to end, well under a millisecond — at documents of thousands of entities, not hundreds.**

"End to end" is the whole loop an editor runs, not just materialization:

```text
have: a typed document at op A
get:  op B, a child of A
want: the typed document at B, plus a summary of what changed
```

[edit-latency-benchmark.md](../benchmarks/edit-latency-benchmark.md) is the harness, and its p95 at 10,000 entities is the acceptance gate.
A latency target is a claim about the tail, so a median cannot check it.

## What the assumption buys, and what each purchase costs

Every one of these is a fast path with a correct slow path behind it.
**Correctness never depends on the workload being what this says; only speed does.**

| assumes | buys | falls back to |
|---|---|---|
| a sweep's walked set has one source | seeding from a cached snapshot ([snapshots](snapshots.md#validity-is-decided-at-use-not-at-creation)) | a plain replay |
| a linear history, so every op is an articulation point | dropping superseded sets wholesale, over a dirty list | carrying them, which is quadratic in writes per path |
| the head is one op from a snapshot | `advance_snapshot`, at the cost of one op's writes | recomputing the snapshot |
| an edit touches few entities | a filtered sweep seeded by lookup rather than by walking the snapshot | reading the whole snapshot |
| a merge is rare | the merge path replays in full until a snapshot exists at or below the merge base | nothing; this is the cost |

The last row is the one to keep in view.
A document whose history is genuinely branch-heavy will be slower, and the design says so rather than pretending otherwise.
The reopen condition is in [decisions.md](../decisions.md#a-snapshot-stores-surviving-only-and-its-validity-is-decided-at-use).
It is profiling that shows the fallback firing on a workload that is not linear-heavy.
