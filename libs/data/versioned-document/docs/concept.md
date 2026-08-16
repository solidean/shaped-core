# The versioned-document concept

The design of `vdoc`, end to end.
This document is the authority on *what* the library is and *why* it is shaped this way.
[decisions.md](decisions.md) records the settled choices and what would reopen each of them; [todo/](todo/_index.md) is the ordered plan to build it.

Four goals drive every choice below:

- **Immutable history.** Nothing is ever overwritten or rewritten, only added.
- **Multi-user collaboration.** Two people editing the same document offline must be able to merge afterwards, deterministically.
- **Long-term compatibility, in both directions.** A three-year-old build must open a document written today, and keep working on the parts it understands.
- **A simple, stable core.** Storage stays generic and unchanging; semantics, validation and tooling are layers *above* it that may churn freely.

The central principle: **the storage model knows nothing.**
It stores named values against named properties of named components of named entities.
Everything that gives those names meaning is interpretation, and interpretation lives above storage and may be replaced without touching a stored byte.

---

## The model

A document is a map from entity to entity contents.

```text
document   : entity_id         -> entity
entity     : component_type_id -> component
component  : property_id       -> value
```

An entity holds any number of components, at most one of each type.
A component is a flat bag of named properties.
A value is a single self-describing binary value, which may itself be structured.

Every property therefore has a unique path, and **the property path is the addressable unit of the whole system**:

```text
wall-17/transform/position
wall-17/meta/name
```

Ops assign to paths, conflicts are per-path, permissions are expressed over paths, and diffs are lists of paths.
Nothing smaller is ever addressed — see [Multi-values](#multi-values) for why that granularity is deliberate.

### Entities and components are created implicitly

There is no create operation.
An entity exists as soon as any property beneath it exists, and a component exists as soon as any property beneath it exists.

This is what keeps merges simple: two people who independently start writing to the same entity have not conflicted, they have both contributed properties.

### Everything is an entity

There are no root objects, no document-level settings blocks and no special-cased singletons.
Application concepts that exist "once per document" are ordinary entities carrying an ordinary component.

The storage layer does not enforce the once-ness, and must not.
An application that finds two of something decides for itself whether to take the first, merge them, warn, or ignore the extras — a policy question, answered where the semantics live.

### Entity ids are strings

An `entity_id` is an arbitrary application-chosen string.
The library attaches no structure to it whatsoever.

That leaves the choice where it belongs.
An application that wants globally unique keys puts a uuid in the string.
An application that wants a well-known entity uses a name, and gets a quasi-singleton it can address without a lookup table.
Both are supported uses, and neither is privileged.

Ids are interned in memory, so comparison and hashing are cheap on hot paths and the string bytes exist once.
**Interning is process-local.** A raw interned id must never be serialized or used to hash persistent data; the canonical string bytes are what everything durable commits to.

---

## Values

A value is a **canonically-encoded, self-describing byte sequence**.
Not a tree of nodes, not a variant of owning types, not JSON.

This codec is not frozen: a general-purpose any-value format landing elsewhere could replace it, which would break the on-disk format rather than refactor it.
See [decisions.md](decisions.md#the-codec-starts-in-vdoc-not-in-clean-core) for what that would and would not promise.

```text
value := tag byte | payload
```

| kind      | payload |
|-----------|---------|
| `null`    | empty |
| `boolean` | one byte, exactly `0` or `1` |
| `integer` | 8 bytes, little-endian, two's complement |
| `number`  | 8 bytes, the IEEE-754 binary64 bit pattern verbatim |
| `string`  | `u32` byte length, then UTF-8 bytes, unterminated |
| `bytes`   | `u32` byte length, then the bytes |
| `array`   | `u32` payload byte length, `u32` element count, then the elements back to back |
| `object`  | `u32` payload byte length, `u32` entry count, then entries; an entry is a `u32` key length, the key bytes, then the value |

A length prefix counts **the bytes that follow it** — the data for `string` and `bytes`, the count field plus the entries for `array` and `object`.
One meaning across all four kinds makes skipping a single rule, `5 + prefix`, whatever the tag — see [decisions.md](decisions.md#a-length-prefix-counts-the-bytes-that-follow-it).

The container length prefixes exist so that **skipping a subtree is O(1)**, which is what makes reading one field of a large value cheap and comparing two values a length check followed by a memcmp.

### Equality is byte equality, and that is the whole point

Two values are equal exactly when their bytes are equal.
Hashing is over the bytes.
There is no structural comparison anywhere in the library.

That property is load-bearing far beyond convenience:

- diffing an edit against its parents is a memcmp, not a tree walk;
- content-addressing an op needs no separate canonicalization pass over values;
- deciding whether two concurrent writers *agreed* — the single hottest question the merge layer asks — is a memcmp.

Byte equality is only meaningful if each value has exactly one valid encoding, so the format is **canonical and decoding enforces it**:

- object keys are sorted ascending by byte order, and duplicate keys are invalid;
- a `boolean` payload other than `0` or `1` is invalid;
- container length prefixes must match their contents exactly, with no trailing bytes.

A non-canonical encoding is a **decode error**, not something tolerated.
Tolerating it would silently break equality, hashing, and every merge decision built on them.

Decoding also enforces a maximum nesting depth, so a hostile or corrupt input cannot drive the decoder into unbounded recursion.

### What the format deliberately does not have

- **No float canonicalization.** `NaN` payloads and the two zeroes are stored as written, and two `NaN`s with different payloads are different values.
  An application that cares normalizes before writing, and doing it here would mean rewriting a caller's number behind their back.
- **No shortest-form integers.** An integer is 8 bytes, always, so there is one encoding because there is one width.
- **No UTF-8 validation.** Canonicality is structural, and byte equality does not care whether the bytes are text.
  `string` says what the application means by the bytes; validating that is the application's job, exactly like float normalization — see [decisions.md](decisions.md#decoding-does-not-validate-utf-8).
- **No hash or reference type.** Asset references are ordinary strings — see [Assets are loosely coupled](#assets-are-loosely-coupled-by-design).
- **No round-trip obligation to JSON.** The library can *print* a value as JSON-ish text for a human, and that is a one-way debugging projection.
  JSON is the display metaphor for the model, never its storage.

### Values are small, and stored inline

Real values are a scalar, a small struct, or a short array — a position, a name, a colour, a handful of flags.
So a value's in-memory storage is `cc::small_vector<cc::byte, N>`.

`cc::small_vector` occupies 48 bytes and grows its inline buffer to fill whatever footprint it already has, so `N = 1` already yields roughly 36 inline bytes.
That covers essentially every real value without an allocation, and the ones it does not are correct, just heap-backed.

**Bulk data does not belong in a value.** A mesh, a texture or a point cloud is a blob, referenced by an asset id — see [Assets and blobs](#assets-and-blobs).

---

## Ops and content addressing

The document described above is a **materialized view**.
It is not what is stored.

What is stored is an immutable, content-addressed DAG of **ops**.
An op is a set of property assignments, plus its parents, plus free-form metadata.

```text
op {
    id:      op_id                            a 32-byte BLAKE3 digest of everything below
    parents: [op_id, ...]                     verbatim, in the op's own order
    payload: optional {                       absent only on a skeleton op left behind by pruning
        metadata_bytes:    an encoded value   author, timestamp, description — informational only
        assignment_bytes:  a tag byte, then the assignments
    }
}
```

**An op holds the producer's bytes, and nothing else.**
`metadata` and `assignments` are decoded *views* over those bytes, produced on demand and stored nowhere.
That is a correctness property rather than a memory optimization — [The op is its bytes](#the-op-is-its-bytes) is why.

Metadata never affects document semantics.
It is committed to by the hash — so it cannot be altered after the fact — but nothing interprets it.

### The producer canonicalizes; the hash just hashes bytes

`op_builder` sorts, deduplicates and serializes; the resulting bytes are the op's payload; `op_id` is a plain BLAKE3-256 hash of those exact bytes.

**No load path ever re-serializes.**
Verification re-hashes the bytes as stored and compares.

That is a correctness property, not an optimization.
If verification re-serialized, any change to a formatter, an integer width or a map iteration order would turn a good stored op into a hash mismatch.
A mismatch is indistinguishable from tampering.
With this rule, a re-serialized payload is simply a *different op*, which is exactly what it is.

The hash input, all integers fixed-width little-endian:

```text
u64 length | "vdoc::op/v1"                  domain separation, so an op id can never collide with any other digest we compute
u32 parent count | each parent's 32 bytes    verbatim, in the op's own order
u64 length | metadata bytes
u64 length | assignments bytes
```

The assignment payload opens with a **one-byte encoding tag**, so the assignment encoding can evolve without changing the hashing rule.
An unknown tag is a decode error naming the tag, not a corruption report.

The builder's canonical order is: parents sorted and deduplicated, assignments sorted by `(entity, component, property)`, and no path assigned twice within one op.
Identical content therefore produces an identical `op_id`, whatever order the caller supplied.

Sorting is by **canonical bytes** throughout — the id strings, and for parents the 32 digest bytes.
Neither an interned id nor a digest's in-memory representation may order anything that reaches the hash, or two machines would disagree on the same content.

### The op is its bytes

An op retains what it was read as, and decodes on demand.
It does not keep a decoded assignment list beside those bytes, because holding both means holding two representations that can disagree.

The rule above is what forces this.
If an op held only decoded assignments, verification would have no choice but to hash `encode(decode(bytes))`.
That is re-serialization under a different name, with every failure mode the rule exists to prevent.
Keeping the bytes makes the guarantee structural: there is no encoder anywhere near a loaded op, so no future change to one can reach it.

Three things follow, and the third is the one worth reading twice:

- **An op *could* be stored and relayed without being interpretable.** Byte retention is what would let a build hand on an op whose assignment encoding it cannot read.
  Today it does not: an unknown assignment encoding tag is a decode error.
  [decisions.md](decisions.md#an-unknown-assignment-encoding-tag-is-a-decode-error) records why that door is left open rather than walked through.
- **Write-back is lossless by construction.** Nothing is dropped on save, because nothing was rewritten.
- **An op assigning to component types this build has never heard of round-trips byte-identically.**
  Not by convention, and not because some layer takes care to preserve it — there is simply no code path that could alter it.
  This is the mechanism underneath cross-version and cross-application compatibility, and [compatibility.md](compatibility.md) is where the consequences are worked out.

### Ops write only what changed

`op_builder::build` materializes the touched entities as seen from the op's own parents, and emits an assignment only where the desired value differs from the current one.

So re-setting an unchanged component produces an op with no assignments, and setting one field of a ten-field component writes one property.
This is what keeps history small enough to keep forever, and it is why an op's assignment list is a genuine changelog rather than a snapshot.

### The DAG

Each op names zero or more parents.

| parents | meaning |
|---------|---------|
| none    | a new document |
| one     | extends that history |
| several | merges those histories |

A document is identified by a single op id — its **head**.
Named heads are **refs**, which live in storage rather than in the model.

Materializing a head means walking its transitive parents, collecting assignments, and resolving overwrites.
Every op id resolves deterministically to exactly one document state, forever.

A naive materialization replays everything reachable.
In practice snapshots are cached against ops, and the walk stops at the first cached snapshot it meets — nearly free on the mostly-linear histories real editing produces.

### Overwrites and dominance

If op B is a descendant of op A and both write the same path, B wins.
This is ordinary last-write-wins along a line of history, and it covers almost every write that ever happens.

---

## Multi-values

When two ops write the same path and **neither is an ancestor of the other**, there is no answer to which came later.
The storage layer refuses to invent one.

Both writes survive.
The property becomes multi-valued, and the raw document keeps every surviving `(writer op id, value)` pair.

```text
raw_property = [ (op_a, 10), (op_b, 20) ]
```

Nothing is lost, nothing is guessed, and a later op that writes the path resolves it back to a single value — because that op dominates both.

### The granularity is the whole property

A property is multi-valued or it is not.
Parts *within* a value never conflict independently: two concurrent writers of `transform/position` produce two whole positions, never a merged position with one writer's `x` and the other's `y`.

That coarseness is chosen, not conceded.
Sub-value merging would mean the storage layer understanding value structure, which is exactly the knowledge this design keeps out of it.
A half-and-half position is also a state neither author ever authored.

### Writers that agree still conflict structurally

If two concurrent writers write the *same* bytes, the property is **still** structurally multi-valued.
The storage layer records what happened, and what happened is two independent writes.

The interpretation layer is where that stops mattering.
Parsing sees the writers agree — a memcmp, thanks to canonical values — and collapses the property silently to that value.
It records it in the report's *agreed multi-values* side channel, as a tidy-up hint for a later write.
No diagnostic, no user-visible conflict.

This case is common in practice, it is easy to get wrong, and it has its own tests.

### Genuine disagreement is the policy's problem

When the surviving writers disagree, the parse policy picks.
The default is deterministic and biased toward the local user:

- if exactly one surviving writer is inside the local closure — the set of ops reachable from the local head — that value wins, and a *remote conflict* diagnostic reports it;
- otherwise the smallest op id wins, and a *multi-valued conflict* diagnostic reports it.

Both branches are total and reproducible: the same inputs always produce the same document, on every machine.

---

## Interpretation

Materialization gives a `raw_document`, mirroring the model exactly and understanding none of it:

```text
raw_document  : entity_id         -> raw_entity
raw_entity    : component_type_id -> raw_component
raw_component : property_id       -> raw_property        // one or more (writer, value) pairs
```

Parsing turns that into a typed `document`, and it is cleanly split in two:

- **`parse_policy` goes in** — *how* to interpret: which component types are known, which entities are alive, how genuine conflicts resolve.
  `default_parse_policy` bakes in the conventions below.
- **`parse_report` comes out** — *what* was found: diagnostics, plus the agreed multi-values.

### Parsing never refuses

Parsing is a projection over an immutable DAG.
It mutates nothing, and it has no failure mode.

An unknown component type, an unknown schema version, an unresolvable conflict — each becomes an entry in the report while the rest of the document loads and stays fully editable.
This is what makes cross-version collaboration work at all: a build that does not understand a component is not entitled to refuse the document that contains it.

Diagnostics are **string-free**: a kind plus the ids it concerns.
The path, the kind and the raw document together are enough to recover the specifics, and they localize later without anything being re-parsed.

### Components belong to the application

The library ships **zero components**.

What a component *is* comes from the application, through `component_traits<C>`:

```text
type_name       a stable string, e.g. "Transform"
schema_version  an integer, bumped when the stored shape changes
write(C)     -> the properties to store
parse(raw)   -> an optional C; empty means "drop this component", and it never fails
```

`component_registry` is the runtime, type-erased set of those types.
It can be extended at any time, merged with another, or handed a subset for a test.
Storage and the typed document never depend on the concrete component set — the parser only ever sees a policy.

### Reserved names

A small number of names are owned by the library, and they are **prefixed with `$`**.
Applications must not use `$`-prefixed component types or property names; everything without the sigil is theirs, forever.

| name | where | meaning |
|------|-------|---------|
| `$schema_version` | any component | the schema version its writer stamped |
| `$alive` | any component | deletion; absent means alive |
| `$entity` | a component type | carries entity-level `$alive`; has no C++ struct and applications never see it |

### Deletion is interpretation, not storage

**There is no delete in the storage model.**
Removing data would break every property the immutable history exists to provide.

Deleting sets `$alive` to false, which is an ordinary assignment in an ordinary op.
The parser reads it and does not instantiate the component — or, on `$entity`, the whole entity.

Something is dead only if `$alive` is *unambiguously* false: every surviving writer says false.
A contested `$alive` keeps the thing alive and files a diagnostic, because resurrecting is recoverable and vanishing is not.

Undeleting is just another write.

### Schema evolution

`write` stamps `$schema_version`.
`parse` reads it, migrates from whatever version it finds, and the next write re-stamps at the current version.

A version this build does not know skips that component with a diagnostic, leaving the stored data untouched for a build that does know it.
Stored history is never rewritten and never migrated in place; old versions stay loadable forever.

---

## The typed document is an immutable index

`document` is built once by a parse and **has no mutation API**.
There is no `set`, no `remove`, no `create`.

That is not a limitation to be lifted later.
Edits go through an op and re-materialize, which is the only path that keeps history honest.
Immutability is also what makes a parsed document safe to hand to another thread and hold for as long as it is useful.

The layout follows from being frozen:

- one arena for the whole document, so building is cheap and destruction is a single release;
- an entity table sorted by entity id;
- per component type, two parallel dense arrays sorted by entity id: the ids, and the components.

From which the query surface falls out:

- `get<C>(entity)` — a binary search;
- iterating one component type — a linear scan of contiguous memory;
- iterating two or more — a sorted-merge intersection, no sparse-set machinery and no indirection.

This is deliberately not an entity-component system.
An ECS optimizes for continuous mutation of a live world; a versioned document is built from scratch, queried heavily, and replaced wholesale.
Transient application state belongs outside the document entirely.

Structural sharing — reusing untouched component arrays when re-parsing after a small edit — is a designed-for future.
The layout must not make it impossible; nothing in v1 implements it.

---

## Assets and blobs

Large binary content is not stored in the document.
The document stores a reference:

```text
wall-17/mesh/asset = "meshes/wall-panel"
```

Where those bytes live, and how they are found, is not the document's business — see below.
A file *can* carry them, and [versioned-document-file](../../versioned-document-file/docs/format.md) specifies how:

```text
name  ->  asset  ->  part, part, part
                     each part names a blob
```

- A **blob** is content-addressed bytes: deduplicated, immutable, and **shared across assets**.
- An **asset** is `{ kind, metadata, a list of parts }`.
- A **part** names one blob plus its format, under a name that is how it is addressed.

Three consequences of sharing blobs rather than owning them:

- **Identical payloads collapse**, whether by derivation or by accident.
  Two meshes with the same index buffer, an asset re-exported with one changed stream, a compressed variant reusing everything but the pixels.
- **Parts load independently.** A consumer can fetch the header, or one level of detail, without touching the rest.
- **Reclamation is mark-and-sweep from the asset index**, with no reference counts to get wrong.

**Part names are the contract, and position within a name disambiguates.**
A part is addressed by `(name, index)` — so a LOD chain is `("lod", 0..n)`, and a lone mesh is `$main`, the reserved default name a single-part asset costs no ceremony to use.
Whole-list position carries nothing: reordering an asset's parts changes no behaviour.

A singular lookup **errors rather than picking one** where a name is carried by several, because an application that expected one part and silently got the first of three has a bug it cannot see.
The argument, including why this reverses the rule it used to be, is [decisions.md](decisions.md#part-names-are-the-contract-and-position-within-a-name-disambiguates).

### Assets declare what they depend on

An asset may carry a list of asset ids it needs, and reclamation keeps the **closure** of a caller-supplied root set under that list.

The list is **declared, never derived.**
The store does not interpret a blob, so it cannot find a reference buried in one.
The only alternative would be for the application to resolve its whole asset graph before it could ask for anything to be collected.
Declaring it means an application names the assets it wants to keep and the file works out the rest.

It is also **uninterpreted**, which is what lets it name assets that live somewhere else entirely — built-in, procedural, remote — alongside the ones this file holds.
An id resolving to nothing here is simply skipped, because a file is one asset source among many and a dangling entry is the ordinary case rather than a defect.
Cycles are ordinary too.

So reclamation is two levels rather than one: narrow the asset index to the closure of the roots, then mark blobs from what survived and sweep the rest.
**Marking is still from the asset index and only from there**, and blobs are still reachable from no op.

### Assets are loosely coupled by design

Asset ids are plain strings, and the value codec has no reference type on purpose.

A file is only **one** source of assets, and it holds what a user embedded persistently.
Built-in assets, procedurally generated ones, remotely fetched ones and cached ones resolve through entirely different machinery, and a string is the only identifier all of them can share.

An **in-memory document has no concept of assets at all**.
Resolution is a downstream concern: a file hands out a blob source plus a small name → (metadata, blob) translation, and whatever caches, streams and decodes assets is built on top of that, elsewhere.

### The asset mapping is mutable, and remapping is retroactive

This is the design's one deliberate hole in immutability, and it must be preserved.

Blobs are immutable and content-addressed.
The **name → asset mapping is not**.
Re-pointing a name changes what every past version of the document resolves to, retroactively, and that is a **feature**.

It follows that **op ids do not commit to asset content**, and a document is reproducible only relative to an asset resolution.

The alternative — hashing asset bytes into the DAG — would make replacing a placeholder mesh, fixing a texture, or relinking a moved library into a rewrite of history.
That is unusable for real content work.
Nobody may "fix" this later; the strictness would cost the format its purpose.

---

## Validation layers

Validation happens in stages, and **only the first can refuse anything**.

| layer | checks | on failure |
|-------|--------|------------|
| 1 — integrity | hashes, parent availability, DAG shape | the affected op is dropped and reported |
| 2 — op syntax | metadata, assignments, paths decode | the affected op is dropped and reported |
| 3 — schema | component types, schema versions, defaults | that component is skipped, with a diagnostic |
| 4 — semantic | stale references, missing assets, application invariants | reported; the document loads |
| 5 — feature support | component types and versions this build lacks | reported; the rest loads |

Layers 2 through 5 are **non-blocking without exception**.
A semantic issue can reduce what an application can do; it can never stop a document opening, and it can never stop an unrelated part of it being edited.

---

## Compatibility, pruning and recovery

### Forward and backward

Older software opens documents written by newer software.
Unsupported components are isolated and reported; supported ones work normally; nothing is dropped on write-back, because nothing was rewritten.

Teams on different builds keep collaborating, which is the entire reason the storage format holds no schema.
The same property lets two *different applications* share one document while each understands only its own half of the component set.

[compatibility.md](compatibility.md) works all of that out: the three kinds of compatibility, the four mechanisms that produce them, what an application owes in return, and where the guarantee ends.

### Pruning

History may be pruned.
A snapshot attached to an op makes everything behind it removable, trading deep history and synchronization range for size and load time.

A pruned parent leaves a **skeleton op**: its id and its parents, with no payload.
A skeleton is **unverifiable by construction** — there are no bytes to hash — and verification must report that as its own outcome, never as a mismatch.
Reporting a pruned op as tampering would be a false alarm about the one thing the system exists to detect.

### Recovery from an untrusted peer

Because an op id recursively commits to everything behind it, a replica missing history can accept it from anyone at all.

It receives the ops, recomputes their hashes, and checks them against the ids it already expects.
If they match, the history is correct — no trust in the sender required, at any point.

This is what BLAKE3 is for, and [decisions.md](decisions.md) records both that choice and the standing reservation about its cost.

---

## What lives outside the document

The document is not the application's state; it is the part of it worth keeping forever.

Outside it, and deliberately:

- **Selection, viewport camera, tool state, undo cursor** — local, per-user, and not history.
  A file stores this as *workspace* state, which creates no op and never makes a document look unsaved.
- **Projections** — render jobs, acceleration structures, outliner trees, inspectors, diff views.
  All derived, all rebuildable, none stored.
- **Permissions** — enforced by whatever serves shared history, by validating ops against path rules.
  Read-only users, annotation-only users and proposal workflows all fall out of path-based rules, and none of them needs a storage primitive.
