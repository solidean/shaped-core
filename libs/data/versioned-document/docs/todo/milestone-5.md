# Milestone 5 — Assets and blobs

**Status: done.**
Landed as `impl/blob_codec`, `impl/blob_fetch`, `impl/reclaim`, and the `blob_payload_reader` seam on both arms.
Plus the `blob_request` and `reclaim_request` actor messages, and the assets half of the conformance suite.

The starting position was better than this file assumed: milestone 4 had already built the asset/blob **write** path and the **load** path in full.
So the work here was blob read-back, the fetch route, real encoding dispatch, reclamation and name resolution.

Five departures from what is written below, each argued where it is recorded:

- **Assets carry a declared dependency list, and reclamation takes a root set.**
  An addition rather than a correction, and the one thing here that changes the design.
  The store never interprets a blob, so it cannot discover an asset→asset reference; without a declared list an application wanting a precise sweep would have to resolve its whole asset graph first.
  Item 6 below is therefore two levels, not one — see [decisions.md](../decisions.md#asset-dependencies-are-declared-by-the-application-and-reclamation-takes-a-root-set).
- **Decoding runs on the storage thread** while `raw` is the only codec, against item 4's wording.
  The read/decode split ships as the seam a real codec moves at; a scheduler built for an identity function would have nothing to validate it.
  A whole-blob fetch goes through decode even under a byte-addressable codec, so that half of the seam is exercised rather than dead.
- **The in-memory arm defers a fetch to `pump()`**, which narrows a milestone-4 decision about the two arms being indistinguishable.
  The arm that could answer instantly is exactly the one that must not, and a correct caller already had to pump for `SC_THREADS=OFF`.
- **The part-range variant is a byte range within a blob.**
  Read literally, "fetch one part without the rest" is already what `load(hash)` does, since a part *is* one blob — so `load_range` is the capability chunking was actually paid for.
  `publish_changes::removed_assets` landed alongside it, unnamed below.
- **Part addressing was reversed to `(name, index)`**, and `$main` added as the reserved default name.
  A per-part accessor on the store was built and then removed: addressing goes through the record `resolve_asset` hands back, so a caller always works from one snapshot.
  Argued in [decisions.md](../decisions.md#part-names-are-the-contract-and-position-within-a-name-disambiguates).

**Goal.** The content store: an asset index over deduplicated, chunked, shared blobs, plus the async blob source that hands bytes out.

**Why here.** The tables and their load path already exist from milestone 4, so this milestone fills them rather than changing the format.
It is also where the design's one deliberate hole in immutability lives, and that hole only makes sense once history's guarantees are real enough to contrast with.

The design is [concept.md](../concept.md#assets-and-blobs); the tables are [format.md](../../../versioned-document-file/docs/format.md#blobs-and-blob_chunk--the-content-store).
Depends on milestone 4.

---

## The model, restated because it is easy to get subtly wrong

```text
name  ->  asset  ->  part, part, part
                     each part names a blob
```

- A **blob** is content-addressed bytes: immutable, deduplicated, and **shared across assets**.
- An **asset** is `{ kind, metadata, a list of parts }`.
- A **part** is `{ blob hash, format, name }`, addressed by `(name, index)` and defaulting to `$main`.

**The name is the contract, and position within a name disambiguates.**
This reverses what this file originally said — that order was the contract and names were cosmetic — and the argument is
[decisions.md](../decisions.md#part-names-are-the-contract-and-position-within-a-name-disambiguates).

In short: a wrong index silently returns a different part, while a wrong name returns nothing.
The old rule also made *reordering* a format change, which is the version of that mistake nobody notices making.

Blob sharing is the point, not an optimization: it is what makes derived assets, re-exports and coincidentally identical payloads cost one copy.

---

## Work items

### 1. The asset index

`asset_record` — id, kind, ordered parts, metadata — loaded whole at open, with blob-side facts filled in from the blob scan.

- `kind` is load-bearing; `meta` is informational.
- An asset with **no parts** is legal, and means metadata without bytes.
  Do not conflate it with an asset whose blobs are missing.
- An asset naming a hash with no blob row is `asset_blob_missing`; one whose chunks do not add up is `asset_blob_incomplete`.
  Both are load issues, and neither stops the file opening.

### 2. The blob store

- Content-addressed by BLAKE3-256 **over the decoded bytes**, so the identity is of the content, not of how it happens to be stored.
- Chunked, using babel's incremental blob I/O from milestone 0.
  Chunking sidesteps SQLite's per-value ceiling and keeps a multi-gigabyte asset from being one allocation.
- `chunk_count` and `stored_size` are what make a torn write **visible**: there is no nullable data column to mistake for "present".
- `ON DELETE CASCADE` takes a blob's chunks with it, which is why `foreign_keys` must be on and why milestone 0 made that configurable.

**The `encoding` seam**: `raw` is the only encoding v1 writes or reads.
The seam is exercised anyway, with encode and decode a real dispatch rather than a placeholder.
That is what makes adding compression later an additive change with no format migration.
An unknown encoding is a load issue and the blob is skipped, never a failed open.
See [decisions.md](../decisions.md#blobs-ship-raw-only-with-the-encoding-seam-reserved).

### 3. Publishing assets and blobs

Assets and blobs **are** listed on a publish, unlike ops: the store cannot know about bytes sitting in memory.

- A `blob_upload` may carry **no data**, meaning "you already have this", so nothing is read back just to be rewritten.
  Combined with a hash naming no stored blob, that is a publish error — and a tested one.
- `decoded_size` is required when the encoding is not `raw`, because nothing else can recover it.
  The file records the stored size implicitly, and the decoded size is only knowable by decoding.
- Assets and blobs go in the **same transaction** as the ops and refs, because a file must never contain an asset whose blob did not land.

### 4. `blob_source`

The seam whatever resolves assets consumes:

```text
load(blob_hash) -> cc::async<bytes>     the DECODED bytes
```

- Held by shared pointer, so a lazy registration keeps the storage alive; `store::close()` severs it and later loads complete with an error.
- **It must not block, and must not re-enter its caller.** It may be called with a caller's internal lock held, so enqueue-and-return is the only correct implementation.
- Decoding happens behind this call, never on the storage thread.
- A part-range variant, so a consumer can fetch one part without the rest.

The store never interprets a blob, and `format` selects a parser somewhere else entirely.

### 5. Name resolution, and where it stops

The file offers a small translation: an asset id string → its metadata and its parts, with a blob source to fetch them through.

**And that is where this library stops.**
Caching, eviction, streaming, format dispatch, decode-to-GPU — all downstream, all somebody else's.
A file is one source of assets among many, which is exactly why asset ids are plain strings and the value codec has no reference type.

### 6. Reclamation

Blobs are reachable **from the asset index only**, never from ops.

`reclaim(roots)` is two levels, in one transaction.
Flood-fill the asset closure from the caller's roots through each asset's declared dependencies, and delete the assets outside it.
Then mark every blob named by a *retained* asset, delete the rest, and let the cascade take the chunks.

**Marking is still from the asset index and only from there** — the index is first narrowed by a root set.
An asset remap may legitimately orphan blobs — that is the case this exists for, not a bug to prevent.

Unmapping one name is the separate, cheaper act: `publish_changes::removed_assets`, retroactive like a remap, collecting no bytes.

### 7. The mutable mapping, defended

The name → asset mapping is mutable, and remapping is **retroactive**: it changes what every past version of the document resolves to.

An asset-index edit therefore **creates no op and moves no ref**, and is not undoable through document history.
It is in the same category as a workspace write.

This will look like an oversight to anyone reading the integrity guarantees in isolation.
It is not — see [decisions.md](../decisions.md#the-asset-mapping-is-mutable-and-remapping-is-retroactive) — and it must not be "fixed" by hashing assets into the DAG.

## API surface this lands

```text
vdoc::file::blob_hash / asset_part / asset_record / blob_upload / blob_upload::of
vdoc::file::main_part_name / part_lookup_error / part_range
vdoc::file::asset_record::main_part() / try_find_part() / part_at() / parts_named() / main_parts()
vdoc::file::blob_source::load / load_range
vdoc::file::reclaim_result / asset_resolution
vdoc::file::store::assets() / make_blob_source() / pump()
vdoc::file::store::reclaim() / resolve_asset()
vdoc::file::publish_changes::removed_assets
```

## Tests

Extending milestone 4's conformance suite, so both store implementations are held to all of it.

- **Dedup**: two assets publishing identical bytes store one blob; the second upload writes nothing.
- **Sharing survives**: deleting one of the two assets leaves the other's blob intact.
- **Ordered parts** round-trip exactly, including an asset with no parts, and one with parts sharing a blob with another asset.
- **Part names are the contract**: a unique name resolves wherever it sits in the list, a shared name is `ambiguous` rather than the first, and an absent one is `not_found`.
  This test is the rule's enforcement.
- **`$main` costs no ceremony**: a part published with no name round-trips as `$main` and is reached through `main_part()`.
- **Several `$main` parts** are reported at load, kept, and error on lookup rather than resolving to one.
- **Chunking**: a blob spanning several chunks round-trips; reading at an offset and across a boundary is correct.
- **Torn blob**: delete one chunk row behind the store's back; the asset reports `asset_blob_incomplete` and the file still opens.
- **Empty upload**: with the blob present it is a no-op; with the blob absent it is a publish error.
- **Unknown encoding**: a blob row naming one is skipped with an issue, and the rest of the file loads.
- **Blob source after close**: loads complete with an error rather than hanging.
- **Blob source under a held lock**: calling `load` while holding a caller lock does not deadlock — the enqueue-and-return contract, tested rather than trusted.
- **Reclamation**: orphaned blobs are collected, referenced ones never are, and an asset remap that orphans a blob is a normal outcome.
- **Retroactive remap**: re-point an asset name, materialize an *old* head, and confirm it resolves to the new content.
  The behaviour is deliberate, so it is pinned by a test rather than left to be discovered.

## Acceptance

- Identical bytes are stored once, whatever route they arrive by.
- No code path anywhere keys on a part's position in the whole list.
- The encoding seam is real dispatch, with `raw` as its only registered entry.
- The blob source never blocks and never re-enters its caller.
- Reclamation marks from the asset index, and only from there.
- [structure.md](../structure.md)'s assets entry is `[done]`, and versioned-document-file's cheat-sheet has lost its `[planned]` banner for these sections.
