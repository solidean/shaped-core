# Concept: assets and blobs

Large binary content is not stored in the document.
The document stores a reference:

```text
wall-17/mesh/asset = "meshes/wall-panel"
```

Where those bytes live, and how they are found, is not the document's business — see [below](#assets-are-loosely-coupled-by-design).
A file *can* carry them, and [versioned-document-file](../../../versioned-document-file/docs/format.md) specifies how:

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
The argument, including why this reverses the rule it used to be, is [decisions.md](../decisions.md#part-names-are-the-contract-and-position-within-a-name-disambiguates).

Blobs ship **raw**, with the encoding seam reserved rather than used — see [decisions.md](../decisions.md#blobs-ship-raw-only-with-the-encoding-seam-reserved).
A file naming an encoding this build has no codec for skips that blob with an issue, and never fails the open.

## Assets declare what they depend on

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

## Assets are loosely coupled by design

Asset ids are plain strings, and the [value codec](values.md) has no reference type on purpose.

A file is only **one** source of assets, and it holds what a user embedded persistently.
Built-in assets, procedurally generated ones, remotely fetched ones and cached ones resolve through entirely different machinery, and a string is the only identifier all of them can share.

An **in-memory document has no concept of assets at all**.
Resolution is a downstream concern: a file hands out a blob source plus a small name → (metadata, blob) translation, and whatever caches, streams and decodes assets is built on top of that, elsewhere.

## The asset mapping is mutable, and remapping is retroactive

This is the design's one deliberate hole in immutability, and it must be preserved.

Blobs are immutable and content-addressed.
The **name → asset mapping is not**.
Re-pointing a name changes what every past version of the document resolves to, retroactively, and that is a **feature**.

It follows that **op ids do not commit to asset content**, and a document is reproducible only relative to an asset resolution.

The alternative — hashing asset bytes into the DAG — would make replacing a placeholder mesh, fixing a texture, or relinking a moved library into a rewrite of history.
That is unusable for real content work.
Nobody may "fix" this later; the strictness would cost the format its purpose.
