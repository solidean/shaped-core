# Milestone 2 — Ids, ops and the DAG

**Goal.** The storage model itself: interned id types, the canonical op encoding, BLAKE3 content addressing, the graph, and materialization into a `raw_document` with multi-values preserved.

**Why here.** This is the layer everything durable commits to.
Its encoding and its hashing rule can never be changed once a file exists in the wild.
So they are built before anything that depends on them, and pinned by tests that read like a specification.

The design is [concept.md](../concept.md#ops-and-content-addressing) and [Multi-values](../concept.md#multi-values).
Depends on milestones 0 and 1.

---

## Work items

### 1. Identity

`entity_id`, `component_type_id` and `property_id`, each a distinct wrapper over `cc::interned_string` so they cannot be mixed up at a call site.

- `of(cc::string_view)` to intern, `as_string_view()` to get the canonical bytes back.
- A default-constructed id is the empty id, and is valid.
- Ordering is **by canonical bytes**, never by the interned id — otherwise every sorted structure in a document becomes run-dependent, and a document would materialize differently on two machines.

### 2. The op

```text
op {
    op_id             id          32-byte BLAKE3
    [op_id]           parents     sorted, deduplicated by the builder
    value             metadata    an object; informational, but hashed
    [assignment]      assignments sorted by (entity, component, property)
    optional<payload> payload     the canonical bytes, absent on skeleton ops
}
```

An `assignment` is `(entity_id, component_type_id, property_id, value)`.

### 3. Canonical encoding and hashing

The hash input, all integers fixed-width little-endian:

```text
u64 length | "vdoc::op/v1"                 domain separation
u32 parent count | each parent's 32 bytes   verbatim, in the op's own order
u64 length | metadata bytes
u64 length | assignments bytes
```

The assignment payload opens with a **one-byte encoding tag**, so the assignment encoding can change without touching the hashing rule.
Tag `1` is: `u32` count, then per assignment the three ids as `u32` length plus bytes, then the encoded value.

**Canonicalization is the producer's job; the hash is a plain hash of the bytes.**
No load path ever re-serializes — verification re-hashes what was read.
If verification re-serialized, a change to a formatter, an integer width or a map iteration order would turn a good stored op into a hash mismatch, and a mismatch is indistinguishable from tampering.

`try_decode_op` is the **only** route from bytes to an `op`, and it verifies as it goes.
Making it the only route is what stops a future loader forgetting to check.

### 4. `op_builder`

Pure: it holds no graph state of its own.

- `set_parents` / `set_metadata` / `set(entity, component)` / `set_raw(entity, component, property, value)`.
- `build(graph)` materializes **only the touched entities** as seen from the builder's parents, and emits an assignment only where the value actually differs.
- Sorts and deduplicates parents, sorts assignments, and rejects the same path being assigned twice.

Identical content must produce an identical `op_id`, whatever order the caller supplied.

### 5. `op_graph`

- `add(op) -> op_id`, keyed by content hash.
  **Idempotent, and not an append**: re-adding a hash already present changes nothing and moves no head.
  Heads are storage's concern, not the graph's.
- `find(op_id)`.
- `children(op_id)` — the inverted parent edges, needed to walk *downstream* of an op, which the parent-only `op` cannot answer.
  Keys may name ops not present, since a child can arrive before its parent.
- `collect_reachable(heads)` — the local closure, used by conflict resolution to tell local ops from remote ones, and by publishing to derive its op set.
  Missing ops are skipped, not errors.
- `materialize(head)` and `materialize(span<op_id>)` for a merge.
- `materialize_entities(heads, entities)` — the cheap path `op_builder::build` uses to diff.

### 6. Materialization and multi-values

Walk the reachable history, collect assignments, resolve overwrites by dominance.

A property keeps **every surviving `(writer op id, value)` pair**.
One value is the normal case; several mean concurrent writers where neither dominates the other.

**Two writers with byte-identical values still leave two entries.**
Storage records what happened, and what happened is two independent writes.
Collapsing here would be interpretation, and interpretation belongs one layer up.

The result is the `raw_document` / `raw_entity` / `raw_component` / `raw_property` chain — proper structs, each wrapping its container, so they are forward-declarable and cannot be interchanged.

Snapshot-terminated materialization is milestone 6; this milestone replays the full history every time.
That is correct and slow, and it must stay correct when the cache lands — which is what the milestone-6 tests check against.

## API surface this lands

```text
vdoc::entity_id / component_type_id / property_id
vdoc::op_id / assignment / op_payload / op
vdoc::op_builder
vdoc::op_graph
vdoc::op_decode_error / op_verification
vdoc::try_decode_op / verify_op
vdoc::raw_document / raw_entity / raw_component / raw_property / property_value
```

## Tests

- **Content addressing**: identical content built in different orders gives one id; any single-byte change gives a different one; parent order is canonicalized.
- **Verification never re-serializes**: decode an op, mutate one stored byte, and the mismatch is detected.
  Then confirm the verify path calls no encoder at all — a structural property, so assert it structurally rather than by inspection.
- **A skeleton op** — id and parents only — reports as *unverifiable*, never as a mismatch.
  This is the false-alarm case that matters most, so it exists from the first milestone that can express it, not from milestone 6.
- **Diffing**: re-setting an unchanged component produces an op with zero assignments; changing one field produces exactly one.
- **Idempotent add**: adding the same op twice leaves one entry and does not disturb the child index.
- **Materialization**: linear history resolves last-write-wins, and a diamond where both sides write the same path leaves two values.
  A diamond where both write the *same bytes* still leaves two values, and that is asserted explicitly.
- **Merge**: materializing several heads equals materializing a merge op over them, for the properties neither side contests.
- **Determinism**: the same DAG materializes byte-identically across runs, and across intern orders.
  Build the same document with the ids interned in a different sequence, and compare.
- **Missing ops**: reachability over a DAG with a pruned parent skips rather than fails.
- **Fuzz** `try_decode_op` on malformed payloads: no crash, no hang, no out-of-bounds read, and never a decoded op that fails its own verification.

## Acceptance

- `try_decode_op` is the only public route from bytes to an `op`, and it always verifies.
- No encoder is reachable from the load path.
- Materialization is deterministic across runs, machines and intern orders.
- Equal-value concurrent writes are preserved as multi-valued, with a test that says so in as many words.
- [structure.md](../structure.md)'s ids / op / op_graph / raw_document entries are `[done]`.
