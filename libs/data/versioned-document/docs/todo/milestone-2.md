# Milestone 2 — Ids, ops and the DAG

**Status: done.**
Landed as `ids.hh`, `op.hh/.cc`, `op_graph.hh/.cc`, `op_builder.hh/.cc` and `raw_document.hh/.cc`, with `ids-test`, `op-test`, `op-fuzz-test`, `op_graph-test` and `op_builder-test`.

The sketch below was **rewritten before implementation** rather than annotated afterwards, because the op's shape changed while the milestone was still a plan.
What the rewrite settled, each recorded in [decisions.md](../decisions.md):

- **The op holds its bytes and decodes on demand.**
  The original sketch gave it decoded `metadata` and `assignments` *plus* an optional payload — a fourth copy of state, matching nothing storage hands back.
  See [decisions.md](../decisions.md#the-op-retains-its-bytes-and-decodes-on-demand).
- **`op_id` orders by its canonical 32 bytes**, never by `cc::hash256`'s defaulted `<=>` — [decisions.md](../decisions.md#op_id-orders-by-its-canonical-32-bytes).
- **A multi-valued property always differs**, so the diff emits even when the surviving writers agree — [decisions.md](../decisions.md#a-multi-valued-property-always-differs).
- **Dominance resolves by propagating a superseded set**, not by ancestor queries — [decisions.md](../decisions.md#dominance-is-resolved-by-propagating-a-superseded-set-not-by-ancestor-queries).
- **Metadata is any canonical value**, not necessarily an object, which the sketch had assumed — [decisions.md](../decisions.md#metadata-is-any-canonical-value-not-necessarily-an-object).

Four things emerged during implementation and are worth carrying forward:

- **`cc::string_view::compare` ordered by *signed* `char`**, so any byte >= 0x80 sorted ahead of all ASCII.
  Milestone 1 met this on object keys and worked around it locally; ids are arbitrary application strings whose sort feeds the op hash, so this time it was fixed in clean-core.
  `vdoc::impl::compare_key_bytes` survives only as a name for the format's ordering.
- **`op_decode_error` has no `malformed_parents` or `count_mismatch`.**
  Both were declared and turned out unreachable from this layer: a bad count surfaces as `truncated` or `trailing_bytes` first, and the parents blob is decoded by the *file* layer.
  Milestone 4 adds the one it needs rather than inheriting dead API.
- **`op_builder::set(entity, component)` is deliberately absent.**
  It is generic over `component_traits`, which is milestone 3; `set_raw` is the layer beneath it and exists now.
- **Milestone 6's snapshot-eligibility rule was corrected here**, before anything could be built on it.
  The articulation point where superseded sets may be dropped is a *per-sweep* property and does not survive being stored.
  [milestone-6.md](milestone-6.md#1-snapshot-terminated-materialization) works out what each snapshot kind must do instead.

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
op_payload {
    metadata_bytes      an encoded value; informational, but hashed
    assignment_bytes    a one-byte encoding tag, then the assignments
}
op {
    op_id                    id       32-byte BLAKE3
    [op_id]                  parents  sorted and deduplicated by the builder
    optional<op_payload>     payload  absent only on a skeleton op
}
```

**The op holds bytes, not decoded content.**
`metadata()` returns a `value_view` over `metadata_bytes`; `assignments()` returns a forward cursor over `assignment_bytes`.
Neither stores anything, and an `assignment` — `(entity_id, component_type_id, property_id, value_view)` — exists only as a cursor's current position.
`optional` therefore carries exactly one meaning — *this op was pruned* — rather than doubling as "we did not keep the bytes".

That is what makes "no load path ever re-serializes" structural rather than a rule to remember.
No encoder is reachable from a loaded op, so no future change to one can turn a good stored op into a mismatch.
The reasoning, and the alternatives rejected, are [decisions.md](../decisions.md#the-op-retains-its-bytes-and-decodes-on-demand).

`try_decode_assignments()` is the eager form, for tests and callers that want the whole list.

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
It rejects unsorted or duplicated parents, and an unknown assignment encoding tag with an error naming the tag.

**Sorting is by canonical bytes everywhere it reaches the hash** — the id strings, and for parents the 32 digest bytes.
`cc::hash256`'s defaulted `<=>` orders four `u64` limbs assembled little-endian, which is *not* the order of those 32 bytes.
A reader implementing the format from this document would sort by bytes and compute a different `op_id`, and that is an interop break no later version can fix.
`op_id::compare_bytes` is the single definition — [decisions.md](../decisions.md#op_id-orders-by-its-canonical-32-bytes).

### 4. `op_builder`

Pure: it holds no graph state of its own.

- `set_parents` / `set_metadata` / `set(entity, component)` / `set_raw(entity, component, property, value)`.
- `build(graph)` materializes **only the touched entities** as seen from the builder's parents, and emits an assignment only where the value actually differs.
- Sorts and deduplicates parents, sorts assignments, and rejects the same path being assigned twice.

Identical content must produce an identical `op_id`, whatever order the caller supplied.

"Differs" is a four-case predicate, and the last case is the one worth stating:

| current state of the path | emit? |
|---------------------------|-------|
| absent | yes |
| one surviving writer, bytes equal | no |
| one surviving writer, bytes differ | yes |
| **two or more surviving writers** | **yes, whatever the bytes** |

A multi-valued property is two independent writes, not one value, so nothing about it equals a single desired value.
This is also the only way a user ever collapses a conflict: the op that resolves a multi-value back to one value *is* an ordinary write of the value already displayed.
Skipping it because the writers happened to agree would import the parse layer's collapse into storage — [decisions.md](../decisions.md#a-multi-valued-property-always-differs).

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

Walk the reachable history in topological order, propagating a per-path state, and resolve overwrites by dominance.

**A state entry is two writer sets: `surviving`, and `superseded`.**
The rule, and the ancestor-query approaches rejected against it, are [decisions.md](../decisions.md#dominance-is-resolved-by-propagating-a-superseded-set-not-by-ancestor-queries).

- Applying op X to path p: `superseded |= surviving`, then `surviving = {X}`.
- Merging parents at p: union both sides' `surviving` and both sides' `superseded`, then drop from `surviving` anything in the combined `superseded`.

The second set is what makes this work without any global ancestor query.
Plain union is wrong: root R writes p, branch L writes p, branch M writes nothing, and the merge would report `{L, R}` when R is L's ancestor and only L survives.
Carrying what each side has already superseded resolves that locally.
It generalizes to nested diamonds, criss-cross merges and n-ary merges, because each parent's ancestor set is ancestor-closed — so a dominating pair always lands entirely inside one parent.

`superseded` is **monotone**: union it forward, never subtract an entry.
Dropping entries that no side currently lists as surviving resurrects a dominated writer, which is the subtle failure this design has to be tested against.

It may, however, be dropped wholesale at an **articulation point**: a moment in the sweep where exactly one live state remains.
Every op still to be processed then descends from it, so no later merge *in this sweep* can present a branch carrying a writer it would have suppressed.
On the mostly-linear histories real editing produces that is nearly every op, so states degenerate to one writer per path.

**The justification is per-sweep, and does not survive being stored.**
A later sweep over a DAG that has grown recomputes the predicate, and an op that qualified before may not qualify now.
That is exactly why dropping it is safe here, and why [milestone 6](milestone-6.md) may not reuse the predicate to decide where a snapshot may be attached.

A property keeps **every surviving `(writer op id, value)` pair**.
One value is the normal case; several mean concurrent writers where neither dominates the other.

**Two writers with byte-identical values still leave two entries.**
Storage records what happened, and what happened is two independent writes.
Collapsing here would be interpretation, and interpretation belongs one layer up.

The result is the `raw_document` / `raw_entity` / `raw_component` / `raw_property` chain — proper structs, each wrapping its container, so they are forward-declarable and cannot be interchanged.

`materialize_entities` is this same pass with an entity filter, applied at exactly two points: where paths are interned, and where an op's assignments are applied.
The filter is a predicate on **assignments, never on edges**.
Filtering edges would sever ancestry and fabricate multi-values, which is the same damage deleting an op outright would do.

Two rules keep the output reproducible.
**No output is ever produced by iterating a hash container** — build a `cc::vector` and sort it with an explicit byte comparator.
And `cc::interned_string::compare_identity` must not be reachable from anything that reaches output, since it is a pointer order that differs every run.

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
- **`op_id` ordering agrees with lexicographic ordering of its hex string**, which pins the parent sort that feeds the hash against the `hash256` limb-order trap.
- **Verification never re-serializes**: decode an op, mutate one stored byte, and the mismatch is detected.
  Then confirm the verify path calls no encoder at all — a structural property, so assert it structurally rather than by inspection.
- **A skeleton op** — id and parents only — reports as *unverifiable*, never as a mismatch.
  This is the false-alarm case that matters most, so it exists from the first milestone that can express it, not from milestone 6.
- **Diffing**: re-setting an unchanged component produces an op with zero assignments; changing one field produces exactly one.
  A multi-valued path emits even when every surviving writer holds identical bytes, and the op that emits collapses it — so a second `build` against that op emits nothing.
- **Idempotent add**: adding the same op twice leaves one entry and does not disturb the child index.
- **Materialization**: linear history resolves last-write-wins, and a diamond where both sides write the same path leaves two values.
  A diamond where both write the *same bytes* still leaves two values, and that is asserted explicitly.
- **Resurrection across a second merge**: A and B concurrently write p; X merges them *and* writes p; Y merges them and writes nothing; Z merges X and Y and must see only `{X}`.
  An implementation that supersedes on the apply step but drops `superseded` at merges passes the plain diamond and fails this, which is why it is a named test rather than a case of the one above.
- **Merge**: materializing several heads equals materializing a merge op over them, for the properties neither side contests.
  And `materialize({A, B})` where A is an ancestor of B equals `materialize({B})` — the multi-head combine is a separate path from the per-op merge, and is where a naive union gets written.
- **A brute-force oracle**, living in the test file: for each writer of a path, walk downstream for a descendant that also writes it.
  Cross-check it against the real pass over a generated small-DAG corpus, so milestone 6's oracle is itself verified rather than trusted.
- **The filter is equivalent to filtering the result**: `materialize_entities(heads, entities) == filter_entities(materialize(heads), entities)` over that corpus.
- **Articulation-point clearing changes nothing**: run the corpus with `superseded` pruning on and off and compare byte for byte.
  This is the same shape as milestone 6's cache-equivalence test, and shares its harness.
- **Determinism**: the same DAG materializes byte-identically across runs, and across intern orders.
  Build the same document with the ids interned in a different sequence, and compare.
  Then shuffle **insertion** order too — head order, within-op assignment order, topological tie-breaks — and add unrelated entities to shift the load factor.
  `hash(interned_string)` is the precomputed hash of the bytes, so a bucket index does not move under a different intern order at all — only probe placement within a chain does.
  The intern-order test therefore catches `compare_identity` misuse and essentially nothing else, and must not stand in for this one.
- **Missing ops**: reachability over a DAG with a pruned parent skips rather than fails.
- **Cycles** from a hostile op set terminate rather than hang.
- **Fuzz** `try_decode_op` on malformed payloads: no crash, no hang, no out-of-bounds read, and never a decoded op that fails its own verification.
  The corpus includes unsorted and duplicated parents, which must be rejected rather than corrupt the child refcounts downstream.

## Acceptance

- `try_decode_op` is the only public route from bytes to an `op`, and it always verifies.
- No encoder is reachable from the load path — an op holds its bytes and decodes on demand, so there is none to reach.
- Everything that feeds the hash sorts by canonical bytes, and a test pins `op_id` ordering against its hex form.
- Materialization is deterministic across runs, machines and intern orders.
- Equal-value concurrent writes are preserved as multi-valued, with a test that says so in as many words.
- [structure.md](../structure.md)'s ids / op / op_graph / raw_document entries are `[done]`.
