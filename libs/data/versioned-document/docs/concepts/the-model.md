# Concept: the model

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
Nothing smaller is ever addressed — see [multi-values](multi-values.md) for why that granularity is deliberate.

The central principle underneath all of it: **the storage model knows nothing.**
It stores named values against named properties of named components of named entities.
Everything that gives those names meaning is [interpretation](interpretation.md), and interpretation lives above storage and may be replaced without touching a stored byte.

## Entities and components are created implicitly

There is no create operation.
An entity exists as soon as any property beneath it exists, and a component exists as soon as any property beneath it exists.

This is what keeps merges simple: two people who independently start writing to the same entity have not conflicted, they have both contributed properties.

## Everything is an entity

There are no root objects, no document-level settings blocks and no special-cased singletons.
Application concepts that exist "once per document" are ordinary entities carrying an ordinary component.

The storage layer does not enforce the once-ness, and must not.
An application that finds two of something decides for itself whether to take the first, merge them, warn, or ignore the extras — a policy question, answered where the semantics live.

## Entity ids are strings

An `entity_id` is an arbitrary application-chosen string.
The library attaches no structure to it whatsoever.

That leaves the choice where it belongs.
An application that wants globally unique keys puts a uuid in the string.
An application that wants a well-known entity uses a name, and gets a quasi-singleton it can address without a lookup table.
Both are supported uses, and neither is privileged.

Ids are interned in memory, so comparison and hashing are cheap on hot paths and the string bytes exist once.
**Interning is process-local.** A raw interned id must never be serialized or used to hash persistent data; the canonical string bytes are what everything durable commits to.
