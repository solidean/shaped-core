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
