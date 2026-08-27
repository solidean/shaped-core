# Per-op edit latency

What one user action costs against a document that already exists, stage by stage, as a distribution rather than a mean.

Source: [tests/benchmarks/edit-latency-benchmark.cc](../../tests/benchmarks/edit-latency-benchmark.cc).

This is the acceptance harness for the incremental edit path.
It measures a different thing from the [document loop benchmark](../../../versioned-document-file/docs/benchmarks/document-loop-benchmark.md).
That one times a whole open / edit / save loop through a file, in bursts of fifty ops.

A gizmo does not run a burst.
It runs one op per frame and needs the answer before the frame ends, so the number that matters is a **per-op latency distribution**, with no file layer in the way.

The target is [workloads](../concepts/workloads.md): a linear op touching a handful of entities, well under a millisecond end to end, at documents of thousands of entities.

## What is measured

Four stages, timed separately on every op:

| stage | what it is |
|---|---|
| `build` | `op_builder::build(graph, cache)` — the diff of the staged edit against its parents |
| `add` | `op_graph::add` |
| `advance` | `advance_snapshot` — rolling the pinned snapshot onto the new head |
| `apply` | `vdoc::apply` — the typed document at the new head, plus the change summary |

`TOTAL` is their sum per op, so its percentiles are of the whole thing an application waits for rather than of four independent distributions.

Three shapes, because real editing produces the first and can produce either of the other two:

- **linear** — each op extends the previous one, which is ordinary editing;
- **drag as a fan** — every frame branches from the *same* state, and only the last becomes history;
- **the same drag chained** — each frame extends the previous one, and the intermediates are dropped on release.

The two drag shapes are **indistinguishable to a user** and cost very differently, which is why both are here.

A snapshot is installed and pinned at the seeded head before anything is measured, which is what an application does immediately after a load.
The policy is `create_with_registry` rather than `create_with_local_head`, deliberately.
Collecting the local closure is a walk of the whole history, and a session that built one per frame would pay that before anything else in the loop.

## Results

Times in milliseconds.
The document is *N* entities, each with six properties, from a linear history of *N* ops.
The edit moves one existing entity — one property, one entity, the smallest real op there is.

| entities | shape | build p95 | advance p95 | apply p95 | **total p50 / p95 / max** |
|---|---|---|---|---|---|
| 500 | linear | 0.003 | 0.000 | 0.003 | **0.006 / 0.007 / 0.038** |
| 2,000 | linear | 0.004 | 0.001 | 0.004 | **0.008 / 0.026 / 0.047** |
| 8,000 | linear | 0.006 | 0.001 | 0.005 | **0.010 / 0.012 / 0.054** |
| 500 | drag, fanned | 0.014 | — | 1.074 | **0.893 / 1.079 / 1.163** |
| 2,000 | drag, fanned | 0.020 | — | 4.715 | **3.968 / 4.726 / 5.043** |
| 8,000 | drag, fanned | 0.030 | — | 19.827 | **19.054 / 19.856 / 21.877** |
| 500 | drag, chained | 0.003 | 0.000 | 0.003 | **0.006 / 0.007 / 0.023** |
| 2,000 | drag, chained | 0.003 | 0.000 | 0.003 | **0.006 / 0.008 / 0.022** |
| 8,000 | drag, chained | 0.004 | 0.001 | 0.003 | **0.006 / 0.009 / 0.041** |

`add` is 1–2 µs at every size and is omitted.

### Findings

**The target is met by about two orders of magnitude.**
A single-entity edit at 8,000 entities is **12 µs at p95** and 54 µs at its worst, against a target of "well under 1 ms".

**And it is flat in document size.**
p95 goes 7 µs → 26 µs → 12 µs across a 16× range of documents; the middle figure is noise, not a trend.
Every stage is now bounded by the size of the *edit* rather than by the document or the history.
That is the property that actually matters: a document being edited all day does not get slower.

**A fanned drag costs a full re-parse per frame, and a chained one does not.**
Frame *k+1* is not a descendant of frame *k* — they are siblings — so `apply` cannot take its fast path, and evolving the document from one frame to the next re-parses everything.
At 8,000 entities that is 19.9 ms per frame against 0.009 ms for the same drag chained: a factor of **2,200**, for two arrangements that are identical to a user.

This is the one actionable finding for an application: **chain a drag's frames rather than fanning them.**
The intermediates are still never published, the undo stack still gets one entry, and on release the chain is dropped with `drop_leaf` and one op is built from the branch point.
[workloads.md](../concepts/workloads.md#at-the-typed-layer-chain-the-frames-instead-of-fanning-them) states it where a reader will meet it.

## Where the time went

The baseline, before any of this work, at 8,000 entities:

| stage | before | after |
|---|---|---|
| `build` | 20.3 ms | 0.005 ms |
| materialize + parse, whole document | 20.2 ms | — |
| `apply` (the same result, incrementally) | — | 0.005 ms |
| **total per op** | **39.9 ms** | **0.010 ms** |

Five changes, in the order they landed:

1. **The sweep's per-path state carries side lists**, so the articulation-point clear walks the paths that are dirty rather than all of them.
   A cold materialization of the same document went from 1,099 ms to 45 ms.
   That is the *open* path rather than the edit path, and it is the largest single term anywhere in the library.
2. **A filtered sweep seeds from a snapshot by lookup**, not by walking the whole snapshot and discarding it.
   Nothing visible on its own, and it is what makes the next one a win.
3. **`op_builder::build` takes a `snapshot_cache`**, so the diff terminates one op back instead of replaying the history.
   20.3 ms → 0.04 ms.
4. **`advance_snapshot` rolls the snapshot forward** along a single-parent edge, for the cost of one op's writes.
   The head then stays permanently one op from a snapshot, so step 3's win does not decay over a session.
5. **`vdoc::apply` evolves the typed document** instead of re-materializing and re-parsing it.
   20.2 ms → 0.005 ms.

**Parse was measured before anything was designed, and that mattered.**
At 2,000 entities a full parse was 0.6 ms and at 8,000 it was 2.5 ms — real, and 6% of the loop.
Had the typed layer been rebuilt first, it would have moved 6% of the number.

## System under test

| | |
|---|---|
| CPU | 12th Gen Intel Core i9-12900H (14 cores / 20 threads) |
| Memory | 32 GB |
| OS | Windows 11 Home |
| Compiler | clang 22.1.7, preset `release-clang` |

The same machine as the [document loop benchmark](../../../versioned-document-file/docs/benchmarks/document-loop-benchmark.md), so the two write-ups compose directly.

Read trends, not third decimals.
At these magnitudes a p95 of 7 µs and one of 26 µs are the same answer.

## Layering, per frame

The second harness in the same file measures the shape [layering](../concepts/layering.md) exists for.
A computed base is rewritten wholesale every frame, user overrides sit on top, and forced values above those.

| stage | what it is |
|---|---|
| `produce` | the base layer's `begin_rebuild` … `finish_rebuild`, rewriting every entity |
| `apply` | `layer_stack::apply` — recomposing and re-interpreting what actually moved |
| `rebuild` | `layer_stack::rebuild` on a throwaway stack, for the comparison |

Four of the entities move each frame; the rest are rewritten with identical bytes, which is the case the diff exists for.

| entities | produce p95 | apply p95 | frame p95 | rebuild p95 |
|---|---|---|---|---|
| 500 | 0.34 ms | 0.014 ms | 0.35 ms | 0.51 ms |
| 2,000 | 1.31 ms | 0.011 ms | 1.32 ms | 1.65 ms |
| 8,000 | 6.03 ms | 0.029 ms | 6.06 ms | 7.70 ms |

**`apply` is flat and `rebuild` is not**, which is the property under test.
Composing and re-interpreting costs O(dirty entities × layers), so it does not care how large the document is — 0.03 ms at 8,000 entities against a 7.7 ms recompose, a factor of 250.
If those two ever converge, the composition has started walking the whole document, and no other test would notice.

**`produce` is the frame's real cost, and it is the producer's rather than the stack's.**
Each write walks three nested sorted vectors with an interned-string comparison at every step, at roughly 100 ns.
That is comfortable to a couple of thousand entities and no further.
Past that a producer should write only what moved rather than rebuilding wholesale — `mark_dirty` skips even the byte compares for one that already knows.

Two findings from writing this harness are worth keeping, because both were invisible until measured:

- **The mark-and-sweep in `finish_rebuild` was the whole frame.** Sorting one entry per written property and searching for each cost 3× everything else at 8,000 entities.
  A steady-state frame inserts nothing and writes exactly as many paths as the layer holds, so the sweep is skipped entirely on that condition.
- **Interning and formatting dominated the first version of the harness**, not the layer.
  `entity_id::of(cc::format(...))` per entity per frame attributed the intern table to the thing under test.
  The paths and stable values are now hoisted out of the loop, as a real producer would hold them.

## Reproducing

```bash
uv run dev.py test "bench-vdoc-edit-latency (one op at a time)" --preset release-clang --timeout 0
uv run dev.py test "bench-vdoc-edit-latency (full sweep)" --preset release-clang --timeout 0 --manual
uv run dev.py test "bench-vdoc-layered-frame (three layers, per frame)" --preset release-clang --timeout 0
uv run dev.py test "bench-vdoc-layered-frame (full sweep)" --preset release-clang --timeout 0 --manual
```

The first records the representative point (2,000 entities) as guide metrics; the second prints the table above.
See [docs/guides/perf-results.md](../../../../../docs/guides/perf-results.md) for the pgo-benchmark mechanism.

## Method notes

**The seed goes around `op_builder`.**
Building a history through the builder was quadratic when this benchmark was written, because every build materialized its parents.
The seed is setup rather than the thing being measured, so it encodes and hashes ops directly, the way the test corpus does.
That is no longer necessary and is kept anyway: setup should not depend on the thing under test.

**The fanned drag runs before the chained one, against the same graph.**
Its frames are dropped in between, so the chained run does not inherit a wider graph — and the drop is itself checked, since it is what stops a session accumulating every frame ever drawn.
