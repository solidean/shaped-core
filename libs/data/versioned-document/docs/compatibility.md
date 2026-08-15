# Compatibility

Three-year-old builds open documents written today.
Today's build opens documents written three years from now, and keeps working on the parts it understands.
Two *different applications* share one document while each understands only its own half of it.

That third one is the interesting claim, and it is what this document exists to state.
The design is [concept.md](concept.md); this is the property that most of it was shaped to buy.

---

## Why the core is minimal on purpose

The storage model is entity → component → property over an immutable DAG of ops, and it holds nothing semantic: no schema, no component list, no type registry.
[The library ships zero components](decisions.md#the-library-ships-zero-components) — every one named in this document belongs to some application, and none of them is `vdoc`'s.

**Nothing storage knows can go out of date, because it knows nothing that could.**
That is what lets the core stay fixed while application logic evolves indefinitely, and everything that *can* age lives in [interpretation](concept.md#interpretation) above it.

So the compatibility surface between two builds is not "do they agree on the format".
It is "which components do they *both* understand" — and that question has a useful answer other than *all* or *none*.

---

## Three kinds, and the third is the point

**Backward.** A new build opens an old document.
Ordinary, and every format claims it.

**Forward.** An old build opens a new document, and stays useful.
Unsupported components are reported and skipped; supported ones work normally; the document stays fully editable in the parts this build knows.
Nothing is dropped on write-back, because nothing was rewritten — see [What makes it hold](#what-makes-it-hold).

**Partial.** Two applications share a document while each understands only a subset of its components.
Neither is "old" or "new"; their component sets simply *overlap* rather than nest.

Partial compatibility is not a weaker form of the other two.
It is the general case, and forward compatibility is the special case where one set happens to contain the other.

### The worked example

A DCC tool authors a scene.
Its entities carry `Transform`, `Mesh` and `Material` — and also `EditorNote`, `LayerAssignment` and `AuthoringHistory`, which exist so a human can work on the file.

A 3D runtime loads the same document.
It understands `Transform`, `Mesh` and `Material`, and has never heard of any of the rest.

The runtime loads the scene, renders it, and can write to it.
The authoring components pass through it untouched and unexamined, and the DCC tool opens the result with all of them intact.
The two applications had to agree on exactly one thing: the rendering and interaction components they both act on.

Nobody had to define an interchange subset, write an exporter, or keep a shared schema in sync.
The document is not "a DCC file the runtime can partly read" — it is one document that both applications are first-class editors of, over different parts.

---

## What makes it hold

Four properties, each specified elsewhere, which together are the guarantee.
None of them is a convention that a careless implementation could quietly break.

**1. An op is never re-serialized.**
An op retains the producer's bytes and decodes on demand, so an op assigning to unknown components round-trips byte-identically ([The op is its bytes](concept.md#the-op-is-its-bytes)).

**2. Parsing never refuses.**
An unknown component type, an unknown schema version and an unresolvable conflict are each an entry in the [parse report](concept.md#parsing-never-refuses).
The rest of the document loads and stays editable.
A build that does not understand a component is not entitled to refuse the document that contains it.

**3. Which writes survived is decided on the raw layer.**
Dominance and multi-values are computed over the op DAG, before anything is interpreted, and they need only the property path ([Multi-values](concept.md#multi-values)).
Storage never picks a winner among concurrent writes — it reports them all, and *choosing* is a parse step layered on top.

That ordering is what lets an application merge concurrent edits to components it cannot read.
It never has to answer "what does this value mean" to answer "which of these writes survived".

**4. The container preserves what it does not understand.**
Unknown columns are ignored *and preserved*, and unknown tables are ignored and reported ([format.md](../../versioned-document-file/docs/format.md#identification-and-versioning)).
So a save cycle through an older build does not strip newer state.

---

## What an application must not do

The guarantee is mutual, and these are the obligations that keep it.

- **Do not use `$`-prefixed component types or property names.**
  That namespace belongs to the library ([Reserved names](concept.md#reserved-names)); everything without the sigil is the application's, forever.
- **Do not interpret another application's components.**
  Reading them is fine and often useful; *depending* on their shape re-creates the coupling this design removes, and does it invisibly.
- **Do not rewrite history to tidy up what you do not understand.**
  Deleting an unknown component is not a cleanup, it is data loss for another application.
  [There is no delete](concept.md#deletion-is-interpretation-not-storage) at the storage layer to do it with, either.
- **Do not prune by deleting ops.**
  Pruning leaves [skeleton ops](concept.md#pruning), and removing the nodes outright severs ancestry and changes what the document says.
- **Stamp `$schema_version` and migrate on read.**
  That is what makes your *own* components forward-compatible with your future self ([Schema evolution](concept.md#schema-evolution)).

---

## Where it ends

Compatibility this strong invites over-reading, so the boundaries are worth stating plainly.

**The container format can break.**
A `user_version` higher than a build knows is a hard failure ([format.md](../../versioned-document-file/docs/format.md#identification-and-versioning)).
That is deliberate: the file may use table shapes this build would misread, and guessing is worse than refusing.

Note the asymmetry, because it looks like a contradiction and is not.
An unknown *component* is skipped while the document loads, because storage can carry it correctly without understanding it.
An unknown *container version* refuses, because the layer that would carry it is the one in question.
The rule is that a build declines only what it cannot handle safely, and it can always handle a component it does not know.

**The value encoding is part of that version.**
Replacing `vdoc::value` with a general-purpose any-value format would be a format break rather than a refactor — [decisions.md](decisions.md#the-codec-starts-in-vdoc-not-in-clean-core).

**An unknown assignment encoding tag is a decode error.**
An op whose assignment blob uses an encoding this build predates cannot be read, and is currently not relayed either.
The reasoning is [decisions.md](decisions.md#an-unknown-assignment-encoding-tag-is-a-decode-error).
The byte-first op keeps the door open to relaying it, and the decision records what would reopen the question.

**Compatibility is about structure, not meaning.**
Two applications that both write `Transform` but disagree about handedness are perfectly compatible at every level this document describes, and will still produce a scene neither one wanted.
Agreeing on the components you share is a design conversation, and nothing here substitutes for it.
