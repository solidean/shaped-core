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

Milestones 0 through 5 have landed, and milestone 6 all but its recovery half; everything above them is `[planned]`.
The milestone that lands each piece is named so the two documents stay in step.

## Top-level structure

```text
src/versioned-document/
  fwd.hh          [done]        forward declarations; also the API index
  value           [done]        the binary value codec                        milestone 1
  ids             [done]        entity_id / component_type_id / property_id   milestone 2
  op              [done]        op / op_id / op_builder, canonical encoding    milestone 2
  op_graph        [done]        the DAG, reachability, materialization         milestone 2
  raw_document    [done]        the untyped materialized document              milestone 2
  snapshot_document [done]      a materialized document owning its bytes       milestone 6
  snapshot_cache  [done]        materializations cached against an op          milestone 6
  component       [done]        traits, registry, schema, component_writer     milestone 3
  parse_report    [done]        diagnostics and agreed multi-values            milestone 3
  parse_policy    [done]        property_reader and the policy seam            milestone 3
  parse           [done]        default_parse_policy and the parser            milestone 3
  document        [done]        the typed immutable index                      milestone 3

src/versioned-document-file/
  fwd.hh          [done]        forward declarations; also the API index
  diagnostics     [done]        load_issue_kind, load_issue, load_report        milestone 4
  memory_image    [done]        the in-memory backing, in the same rows         milestone 4
  store           [done]        open / load / verify / refs / publish / close   milestone 4
  workspace       [done]        the disposable side table                       milestone 4
  assets          [done]        the vocabulary, the store and the blob source   milestone 5
  snapshots       [done]        decoded, cached, pinned when required           milestone 6

src/versioned-document-file/impl/
  rows            [done]        one struct per table, untyped                   milestone 4
  store_io        [done]        the reader/writer seam, and the publish job     milestone 4
  load            [done]        the one loader, shared by both implementations  milestone 4
  publish         [done]        the one publisher, shared the same way          milestone 4
  reclaim         [done]        the one reclaimer, shared the same way          milestone 5
  payload_codec   [done]        the encoding table; `raw` is its one entry      milestone 5
  snapshot_codec  [done]        the snapshot-v1 layout, and its decoder         milestone 6
  snapshot_write  [done]        one snapshot write, and one prune               milestone 6
  blob_fetch      [done]        the one route from a blob hash to bytes         milestone 5
  store_memory    [done]        the in-memory arm, and the suite's oracle       milestone 4
  sqlite_schema   [done]        the DDL, the pragmas, the shape check           milestone 4
  sqlite_io       [done]        the only file that talks to a database          milestone 4
  sqlite_actor    [done]        the actor that owns the connection              milestone 4
  store_sqlite    [done]        the file arm                                    milestone 4
```

## Prerequisites in lower libraries [done]

Three lower-library gaps blocked the first milestones.
Each was an addition to the library that owns the capability, not something worked around here — [todo/milestone-0.md](todo/milestone-0.md) carries the detail.

- `[done]` **`cc::blake3`** plus a 256-bit digest type, and the vendored `extern/blake3` behind it.
  Content addressing needs a cryptographic hash; `cc::hash128` is XXH3 and cannot support verification against an untrusted peer.
- `[done]` **`cc::interned_string`** — process-local interning, with the rule that a raw interned id is never serialized.
- `[done]` **`babel::sqlite` additions** — incremental blob I/O, transaction scoping, connection configuration.

A fourth landed with milestone 1: `[done]` **`cc::load_bytes_le` / `cc::store_bytes_le`** and their `_be` twins, in `clean-core/common/endian.hh`.
babel had already filed that gap with three hand-rolled copies, and the value codec would have been the fourth.
babel's readers moved onto it in the same change, so the gap is closed rather than merely covered.

## versioned-document [done]

### value [done]

The canonical binary codec: tag byte plus payload, length-prefixed containers, byte equality, decode-time canonicality enforcement.
Design: [concept.md](concept.md#values). Landed in [milestone 1](todo/milestone-1.md).

- `[done]` **encode / decode / skip** over the eight kinds.
- `[done]` **`value_builder`** for arrays and objects, sorting object keys on build.
- `[done]` **debug text projection** — one-way, JSON-ish, for dumps and test failure output.

### ids, ops and the DAG [done]

Interned id types, the canonical op encoding, BLAKE3 hashing, and the graph that holds them.
Design: [concept.md](concept.md#ops-and-content-addressing). Landed in [milestone 2](todo/milestone-2.md).

- `[done]` **decode-and-verify as the only route from bytes to an op**, so a loader cannot forget to check.
- `[done]` **the op holds its bytes and decodes on demand**, which is what makes "never re-serialize" structural.
- `[done]` **`op_builder` diffing against parents**, emitting only changed properties.
- `[done]` **materialization** of one head or several, with multi-values preserved.
- `[done]` **dominance by propagated superseded sets**, resolving overwrites without any global ancestor query.

### interpretation [done]

The typed layer: registry, traits, policy, report, and the immutable index.
Design: [concept.md](concept.md#interpretation). Landed in [milestone 3](todo/milestone-3.md).

- `[done]` **`default_parse_policy`** — the conventions: registry lookup, `$alive` deletion, local-closure conflict resolution.
- `[done]` **schema versioning** — stamping, migration, and skipping an unknown version with a diagnostic.
- `[done]` **the query surface** — `get`, single-type iteration, and multi-type sorted-merge joins.
- `[done]` **the multi-value rules in one place** — `property_reader::try_get`, which is what a component's parse is handed.
- `[done]` **selection and construction split**, so every structural diagnostic is filed once and the two phases cannot disagree.

## versioned-document-file [in progress]

### the store [done]

The `.vdoc` file: schema, load, verification, refs, publish, workspace, and the actor that owns the connection.
Specification: [format.md](../../versioned-document-file/docs/format.md). Landed in [milestone 4](todo/milestone-4.md).

- `[done]` **two implementations behind one seam** — in-memory and SQLite — with a conformance suite run against both.
- `[done]` **publish derives its op set from refs by reachability**, so an unreachable op cannot be published by mistake.
- `[done]` **the sticky first-failure latch**, so a failing autosave surfaces immediately rather than at close.

### assets and blobs [done]

The asset index over a chunked, deduplicated blob store, plus the async blob source.
Design: [concept.md](concept.md#assets-and-blobs). Landed in [milestone 5](todo/milestone-5.md).

- `[done]` **blob sharing across assets**, with mark-and-sweep reclamation from the asset index.
- `[done]` **declared asset dependencies**, so reclamation takes a root set and computes the closure.
  See [decisions.md](decisions.md#asset-dependencies-are-declared-by-the-application-and-reclamation-takes-a-root-set).
- `[done]` **the encoding seam**, with `raw` the only registered encoding — see [decisions.md](decisions.md#blobs-ship-raw-only-with-the-encoding-seam-reserved).
- `[done]` **the async blob source**, enqueue-and-return, with a byte-range variant over the chunked store.

### snapshots, pruning and recovery [in progress]

Materialization caching, history pruning, skeleton ops, and verifying history received from an untrusted peer.
Lands in [milestone 6](todo/milestone-6.md); recovery is what remains.

- `[done]` **snapshot-terminated materialization**, so a long history costs what a short one does.
- `[done]` **validity decided at use, against today's DAG**, rather than recorded when a snapshot was taken.
- `[done]` **persisted snapshots**, chunked, behind the shared payload codec.
- `[done]` **history pruning**, bounded to the oldest op every ref descends from, and never automatic.
- `[done]` **skeleton ops reported as unverifiable**, never as a hash mismatch.
- `[planned]` **recovery from an untrusted peer**, by recomputing hashes over the bytes as received.
