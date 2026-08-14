# versioned-document structure (vdoc::)

The living roadmap for versioned-document and versioned-document-file.
Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started

Update the tags as milestones land.
This document tracks *state*; it is not where the design lives.

- The design is [concept.md](concept.md).
- The settled choices and their reasoning are [decisions.md](decisions.md).
- The ordered plan, with acceptance criteria per step, is [todo/](todo/_index.md).

Everything below is `[planned]` today.
Nothing is implemented, and the milestone that lands each piece is named so the two documents stay in step.

## Top-level structure

```text
src/versioned-document/
  fwd.hh          [done]        forward declarations; also the API index
  value           [planned]     the binary value codec                        milestone 1
  ids             [planned]     entity_id / component_type_id / property_id   milestone 2
  op              [planned]     op / op_id / op_builder, canonical encoding    milestone 2
  op_graph        [planned]     the DAG, reachability, materialization         milestone 2
  raw_document    [planned]     the untyped materialized document              milestone 2
  component       [planned]     traits, registry, schema                       milestone 3
  parse           [planned]     policy, report, diagnostics, the parser        milestone 3
  document        [planned]     the typed immutable index                      milestone 3

src/versioned-document-file/
  fwd.hh          [done]        forward declarations; also the API index
  store           [planned]     open / load / verify / refs / publish / close   milestone 4
  workspace       [planned]     the disposable side table                       milestone 4
  assets          [planned]     asset index, parts, blob store, blob_source     milestone 5
  snapshots       [planned]     snapshot caching, pruning, skeleton ops         milestone 6
```

## Prerequisites in lower libraries [planned]

Three lower-library gaps block the first milestones.
Each is an addition to the library that owns the capability, not something worked around here — [todo/milestone-0.md](todo/milestone-0.md) carries the detail.

- `[planned]` **`cc::blake3`** plus a 256-bit digest type, and the vendored `extern/blake3` behind it.
  Content addressing needs a cryptographic hash; `cc::hash128` is XXH3 and cannot support verification against an untrusted peer.
- `[planned]` **`cc::interned_string`** — process-local interning, with the rule that a raw interned id is never serialized.
- `[planned]` **`babel::sqlite` additions** — incremental blob I/O, transaction scoping, connection configuration.

## versioned-document [planned]

### value [planned]

The canonical binary codec: tag byte plus payload, length-prefixed containers, byte equality, decode-time canonicality enforcement.
Design: [concept.md](concept.md#values). Lands in [milestone 1](todo/milestone-1.md).

- `[planned]` **encode / decode / skip** over the eight kinds.
- `[planned]` **`value_builder`** for arrays and objects, sorting object keys on build.
- `[planned]` **debug text projection** — one-way, JSON-ish, for dumps and test failure output.

### ids, ops and the DAG [planned]

Interned id types, the canonical op encoding, BLAKE3 hashing, and the graph that holds them.
Design: [concept.md](concept.md#ops-and-content-addressing). Lands in [milestone 2](todo/milestone-2.md).

- `[planned]` **decode-and-verify as the only route from bytes to an op**, so a loader cannot forget to check.
- `[planned]` **`op_builder` diffing against parents**, emitting only changed properties.
- `[planned]` **materialization** of one head or several, with multi-values preserved.

### interpretation [planned]

The typed layer: registry, traits, policy, report, and the immutable index.
Design: [concept.md](concept.md#interpretation). Lands in [milestone 3](todo/milestone-3.md).

- `[planned]` **`default_parse_policy`** — the conventions: registry lookup, `$alive` deletion, local-closure conflict resolution.
- `[planned]` **schema versioning** — stamping, migration, and skipping an unknown version with a diagnostic.
- `[planned]` **the query surface** — `get`, single-type iteration, and multi-type sorted-merge joins.

## versioned-document-file [planned]

### the store [planned]

The `.vdoc` file: schema, load, verification, refs, publish, workspace, and the actor that owns the connection.
Specification: [format.md](../../versioned-document-file/docs/format.md). Lands in [milestone 4](todo/milestone-4.md).

- `[planned]` **two implementations behind one seam** — in-memory and SQLite — with a conformance suite run against both.
- `[planned]` **publish derives its op set from refs by reachability**, so an unreachable op cannot be published by mistake.
- `[planned]` **the sticky first-failure latch**, so a failing autosave surfaces immediately rather than at close.

### assets and blobs [planned]

The asset index over a chunked, deduplicated blob store, plus the async blob source.
Design: [concept.md](concept.md#assets-and-blobs). Lands in [milestone 5](todo/milestone-5.md).

- `[planned]` **blob sharing across assets**, with mark-and-sweep reclamation from the asset index.
- `[planned]` **the encoding seam**, with `raw` the only registered encoding — see [decisions.md](decisions.md#blobs-ship-raw-only-with-the-encoding-seam-reserved).

### snapshots, pruning and recovery [planned]

Materialization caching, history pruning, skeleton ops, and verifying history received from an untrusted peer.
Lands in [milestone 6](todo/milestone-6.md).

- `[planned]` **snapshot-terminated materialization**, so a long history costs what a short one does.
- `[planned]` **skeleton ops reported as unverifiable**, never as a hash mismatch.
