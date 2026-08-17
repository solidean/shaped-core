# Concept: the typed document is an immutable index

`document` is built by a parse and **has no mutation API**.
There is no `set`, no `remove`, no `create`, and there never will be.

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

## Evolving a document

The sentence above stands and has to be read precisely: **a document you are holding cannot change**.
What was also true in v1, and is no longer, is that the only way to a new document was a full parse.

`document_builder` **consumes** a document and produces the next one.
It adopts a `document&&`, patches the arrays in place, and `freeze()`s back into an immutable `document`.
Getting a new one takes the old one away, which is the compile-time form of "no readers right now".
It is the same shape as `op_builder` and `op`: a mutable staging type beside an immutable result.

Immutability was bought for one thing above all: a parsed document is safe to hand to another thread and hold indefinitely.
Consuming evolution keeps that exactly.
A document that is being evolved is not one anyone else holds, because evolving it required giving it up.

**This is what was built instead of structural sharing**, which v1 named as the designed-for future.
Sharing untouched columns by reference is the right answer when *several versions are live at once*.
It costs a refcount per column plus a shared-ownership story for the arena, because components may own heap and a shared column therefore needs shared destruction.
An editor keeps one document and the op graph, not a history of documents, so it would have paid that for nothing.
The layout still does not preclude sharing; nothing here forecloses it.
[decisions.md](../decisions.md#a-document-is-evolved-by-consuming-it-not-by-mutating-it-and-not-by-sharing-it) carries the argument.

Two things the layout needed for this:

- **`component_schema::relocate_range`**, because a dense column cannot insert, remove or grow without moving its tail.
  The library cannot memmove instead: a component owes only being move-constructible and destructible, and nothing generic knows whether it is more.
- **A capacity beside each count.** The arena is a bump allocator with no free, so a column that outgrows its arrays allocates fresh ones and reports the old as dead.
  `document_builder::compact` relocates everything into a new arena when that adds up, which is an order of magnitude cheaper than a re-parse because no value is decoded.

`document_builder` is a **storage** edit and knows nothing about interpretation.
It does not read `$alive`, consult a policy or file a diagnostic.
[`vdoc::apply`](interpretation.md#applying-an-op-incrementally) is what does all three, and drives this underneath.
