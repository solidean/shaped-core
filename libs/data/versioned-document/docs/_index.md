# versioned-document docs

Documentation hub for versioned-document — structured documents that are versioned, mergeable and verifiable (namespace `vdoc`).
Start at the [readme](../readme.md) for the overview, and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

Repo-wide docs are at [docs/_index.md](../../../../docs/_index.md).

## Topics

- [compatibility.md](compatibility.md) — what a document guarantees across builds *and across applications*, and what an application owes in return.
  Backward, forward, and the partial case two different applications sharing one document actually live in.
- [decisions.md](decisions.md) — every settled decision, its reasoning, and what would reopen it.
  Read this before proposing a change that contradicts one, and note that several are recorded as reversals.

## Benchmarks

- [edit-latency-benchmark.md](benchmarks/edit-latency-benchmark.md) — what one user action costs against an existing document, per stage, as a distribution.
  The acceptance harness for the incremental edit path.

## Concepts

The design, one file per question a reader arrives with.
A concept doc answers "what is this, and why is it shaped this way?".
It is not the full API — that is the [cheat-sheet](../cheat-sheet.md) — and it is not a plan; everything here describes what exists.

Read [the model](concepts/the-model.md) first if you are reading only one.

- [the workload](concepts/workloads.md) — the editing shapes this is tuned for, the sub-millisecond acceptance number, and what every fast path falls back to.
  Several decisions elsewhere point here rather than restating it, so read it before arguing that one of them is too optimistic.
- [the model](concepts/the-model.md) — entity → component → property, why entities are created implicitly, and why an entity id is just a string.
  The property path is the addressable unit of the whole system, and this is where that is established.
- [values](concepts/values.md) — the canonical binary codec: the eight kinds, what a length prefix counts, and why equality is byte equality.
  Also what the format deliberately does not have, and why each omission is deliberate.
- [ops and content addressing](concepts/ops-and-content-addressing.md) — the immutable DAG that is the real source of truth, the hash preimage, and the rule that no load path re-serializes.
  Why an op is its bytes, and what that buys for compatibility.
- [multi-values](concepts/multi-values.md) — what happens when two writers, neither dominating the other, write the same path.
  Why the granularity is the whole property, and why writers that agree still conflict structurally.
- [interpretation](concepts/interpretation.md) — the typed layer: policy in, report out, and why parsing never refuses.
  Reserved names, deletion as an ordinary write, schema evolution, and the five validation layers.
- [the typed document](concepts/the-typed-document.md) — why `document` has no mutation API, and the layout that follows from being frozen.
- [layering](concepts/layering.md) — composing several independent histories into one document, where a higher layer replaces a lower one per property path.
  Why property granularity is the requirement rather than a refinement, and why the composition has to happen below the typed layer.
- [assets and blobs](concepts/assets-and-blobs.md) — bulk content beside the document rather than in it, blob sharing, declared dependencies, and the one deliberate hole in immutability.
- [snapshots](concepts/snapshots.md) — how materialization stays independent of history length, why a snapshot stores surviving only, and why its validity is decided at use rather than at creation.
- [pruning and recovery](concepts/pruning-and-recovery.md) — discarding history, and taking it back from a peer nobody trusts.
  The same boundary seen from both sides, including how far a document may prune and why.
- [what lives outside the document](concepts/outside-the-document.md) — selection, camera, projections and permissions, and why none of them is history.

The persistence layer has its own hub: [versioned-document-file](../../versioned-document-file/docs/_index.md).
Its [format.md](../../versioned-document-file/docs/format.md) specifies the on-disk shape.

Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
