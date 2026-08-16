# Concept: the typed document is an immutable index

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
Transient application state belongs [outside the document](outside-the-document.md) entirely.

Structural sharing — reusing untouched component arrays when re-parsing after a small edit — is a designed-for future.
The layout must not make it impossible; nothing in v1 implements it.
