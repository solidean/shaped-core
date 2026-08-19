# Concept: interpretation

Materialization gives a `raw_document`, mirroring [the model](the-model.md) exactly and understanding none of it:

```text
raw_document  : entity_id         -> raw_entity
raw_entity    : component_type_id -> raw_component
raw_component : property_id       -> raw_property        // one or more (writer, value) pairs
```

Parsing turns that into a typed `document`, and it is cleanly split in two:

- **`parse_policy` goes in** — *how* to interpret: which component types are known, which entities are alive, how genuine conflicts resolve.
  `default_parse_policy` bakes in the conventions below.
- **`parse_report` comes out** — *what* was found: diagnostics, plus the agreed multi-values.

## Parsing never refuses

Parsing is a projection over an immutable DAG.
It mutates nothing, and it has no failure mode.

An unknown component type, an unknown schema version, an unresolvable conflict — each becomes an entry in the report while the rest of the document loads and stays fully editable.
This is what makes cross-version collaboration work at all: a build that does not understand a component is not entitled to refuse the document that contains it.

Diagnostics are **string-free**: a kind plus the ids it concerns.
The path, the kind and the raw document together are enough to recover the specifics, and they localize later without anything being re-parsed.

## Components belong to the application

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

## Reserved names

A small number of names are owned by the library, and they are **prefixed with `$`**.
Applications must not use `$`-prefixed component types or property names; everything without the sigil is theirs, forever.

| name | where | meaning |
|------|-------|---------|
| `$schema_version` | any component | the schema version its writer stamped |
| `$alive` | any component | deletion; absent means alive |
| `$entity` | a component type | carries entity-level `$alive`; has no C++ struct and applications never see it |

## Deletion is interpretation, not storage

**There is no delete in the storage model.**
Removing data would break every property the immutable history exists to provide.

Deleting sets `$alive` to false, which is an ordinary assignment in an ordinary op.
The parser reads it and does not instantiate the component — or, on `$entity`, the whole entity.

Something is dead only if `$alive` is *unambiguously* false: every surviving writer says false.
A contested `$alive` keeps the thing alive and files a diagnostic, because resurrecting is recoverable and vanishing is not.

Undeleting is just another write.

## Schema evolution

`write` stamps `$schema_version`.
`parse` reads it, migrates from whatever version it finds, and the next write re-stamps at the current version.

A version this build does not know skips that component with a diagnostic, leaving the stored data untouched for a build that does know it.
Stored history is never rewritten and never migrated in place; old versions stay loadable forever.

## Applying an op incrementally

`vdoc::apply` evolves a typed document from one op to another instead of re-parsing it.
It is the same interpretation, run over fewer entities — the selection phase is one function, and both `parse` and `apply` call it, so the two cannot drift.

**The fast path is a bounded single-parent chain.**
Walking from `to` back to `from`, every op must have exactly one parent, and `from` must be reached inside a bound.
Single-parentage is what makes the chain's own assignments the complete delta.
Each op dominates what it overwrites and contributes nothing else, so no untouched entity changed and none became multi-valued.
Multi-values *inside* the touched set are fine — those entities go through the full selection-and-construction path, exactly as a parse runs it.

The gate is a bound rather than a proof of ancestry, for the same reason the [snapshot](snapshots.md#validity-is-decided-at-use-not-at-creation) gate is.
An exact ancestor test on a DAG is what this design declines to pay for.
Anything else — a merge on the chain, a chain too long, a `to` that does not reach `from` — re-materializes and re-parses.
Correctness never depends on the gate; only speed does.

### The dirty set is the input, and it is allowed to over-report

An apply does not consume a pair of ops; it consumes a **`change_set`** — the property paths that have to be re-interpreted — and the op chain is one way to produce one.

That matters because a chain is not the only thing that knows a delta.
A source written directly knows its own, and a composition of several sources knows the union of theirs, and neither can be phrased as "from this op to that one".

A change set carries a **granularity**: property, component or entity.
Coarser is a superset, and the contract is one-directional:

> `covers` may answer true where nothing changed, and never false where something did.

So coarsening is a pure speed dial.
Every consumer is correct at every granularity, and only the amount of recomputation varies — which means a producer that cannot be precise is free to be coarse and is never forced to be wrong.
Refining is what no one may do, and `coarsen_to` asserts rather than allowing it.

An apply coarsens to entity granularity immediately, because that is what re-interpretation works at.
Selection and construction run one entity at a time, so knowing which property under it changed buys nothing.

### A fallback says why

`incremental_apply_stats::fallback_reason` distinguishes the three ways the fast path declines.
The caller forced it, the history had no single-parent chain, or the chain was longer than `max_chain_ops`.

Only the last is fixed by raising the bound, and only the middle one is a statement about the workload.
Conflating them is how a fallback that should not be happening stays merely slow instead of becoming findable.

### What an incremental apply owes the report

A parse appends.
An apply **edits**, because carrying a stale finding for something it has just re-decided is a correctness trap rather than untidiness.

- Entries naming an entity in the touched set are **dropped before anything re-decides it**, then recomputed.
- On the fallback the whole report is cleared, because a full parse re-decides the whole document.
- **`unsupported_component_type` is never retracted.**
  It is document-scoped: reported once per type, not once per entity, and carrying no entity at all.
  So an apply cannot know a type is *gone* without walking the whole document, which is the one thing this path exists to avoid.
  A type that stops being present keeps its diagnostic until the next full parse.

That asymmetry is stated rather than hidden: it is the one way an incremental report differs from the parse of the same op.

### The change summary

`apply` produces a `change_summary` rather than leaving a caller to diff two documents: touched entities and touched (entity, component) pairs, each marked added, removed or modified.

`modified` means the component was re-parsed because a raw property under it changed.
**It is not a claim that the parsed value differs**, which the library cannot make — a component is required only to be move-constructible and destructible, so there is no equality to test.

On the **fallback** the summary is conservative: everything.
A full re-parse has no delta to read, and a caller seeing `took_fast_path == false` should treat every projection it holds as stale — which is exactly what that summary says.

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

Only layer 1 sits below interpretation, in storage.
The rest are stages of the parse described above, which is why they share its one guarantee: they report, and they never refuse.
