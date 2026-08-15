# The `.vdoc` file format

The on-disk specification.
A `.vdoc` file is a single SQLite database holding a document's whole history, the assets a user embedded in it, and the disposable UI state that goes with it.

The model it stores is [versioned-document](../../versioned-document/docs/concept.md)'s; read that first.
This document is the authority on the bytes.

Everything here is `[planned]` — the specification is complete, the implementation is not.
[milestone-4](../../versioned-document/docs/todo/milestone-4.md) through [milestone-6](../../versioned-document/docs/todo/milestone-6.md) build it.

---

## What a file is for

One file is one shareable unit.
Send it to someone and they have the document, its full history, and the content embedded in it.

The op DAG loads **eagerly and completely** at open; blobs load **lazily**, in pieces, on demand.
That split is the whole performance story: history is small and wanted immediately, content is large and wanted selectively.

Writes are a **write-behind log**.
Publishing appends the ops reachable from committed refs and never renames a file into place, so a crash costs at most the last publish and never the document.

---

## The three durability classes

**Only one of the three is immutable, and that is deliberate.**

| class | tables | guarantee |
|-------|--------|-----------|
| history | `ops`, `refs`, `snapshots` | content-addressed, verified on load, append-only |
| content | `assets`, `blobs`, `blob_chunk` | blobs immutable and content-addressed; **the name → asset mapping is mutable** |
| workspace | `workspace` | none; deleting every row leaves a fully valid document |

Two consequences that must not be "fixed" later:

- **Op ids do not commit to asset content.** A document is reproducible only relative to an asset resolution.
  Re-pointing an asset name changes what every past version resolves to, retroactively — the escape hatch that makes the format usable for real content work.
  The reasoning in full: [decisions.md](../../versioned-document/docs/decisions.md#the-asset-mapping-is-mutable-and-remapping-is-retroactive).
- **An asset-index edit creates no op and moves no ref**, so it is not undoable through document history.
  It is in the same category as a workspace write.

---

## Identification and versioning

```sql
PRAGMA application_id = 0x56444F43;   -- 'VDOC'
PRAGMA user_version   = 1;            -- the format version
```

`application_id` identifies the file as ours to anything inspecting it, `file(1)` included.
`user_version` is the format version.

The compatibility rules:

- A **higher** `user_version` than this build knows is a **hard failure**, because the file may use table shapes this build would misread and guessing is worse than refusing.
- An **equal or lower** version opens, and missing tables are created.
  An old shape that cannot be read forward is reported as a specific error, never left to fail obscurely several statements later.
- **Unknown tables are ignored**, and reported as a load issue, since a newer build may have added state this one does not need.
- **Unknown columns are ignored**, and preserved: a rewrite must never drop a column it did not understand.

`user_version` also covers the **value encoding**, not just the table shapes.
If `vdoc::value` is ever replaced by a general-purpose any-value format, that is a format break, and this is the field that lets a future build tell the two apart and migrate in principle.
Whether a migration is written is undecided — see [decisions.md](../../versioned-document/docs/decisions.md#the-codec-starts-in-vdoc-not-in-clean-core).

---

## Schema

`PRAGMA foreign_keys = ON` — the chunk cascade below depends on it.

### `ops` — the DAG

```sql
CREATE TABLE ops (
    hash        BLOB PRIMARY KEY NOT NULL,  -- 32-byte BLAKE3 op id
    parents     BLOB NOT NULL,              -- 32 bytes per parent, concatenated, in the op's own order
    metadata    BLOB,                       -- an encoded vdoc value (object), verbatim
    assignments BLOB                        -- 1-byte encoding tag + payload, verbatim
) WITHOUT ROWID;
```

`parents` carries no count: it is a whole number of 32-byte ids, and a length that is not a multiple of 32 is a decode error.

`metadata` and `assignments` are stored **exactly as the producer canonicalized them**, and the op id is a plain hash of those bytes plus the parents.
Loading re-hashes what it read; nothing on the load path re-serializes.
This is what stops a formatter change from ever looking like tampering.

Both may be `NULL`, which is a **skeleton op**: a pruned parent, kept for its position in the DAG.
A skeleton is unverifiable by construction and must be reported as such, never as a hash mismatch.

### `refs` — named heads

```sql
CREATE TABLE refs (
    name    TEXT PRIMARY KEY NOT NULL,
    op_hash BLOB NOT NULL
) WITHOUT ROWID;
```

Refs are the roots of reachability.
Publishing derives the op set to persist from the refs being set, so an op no ref can reach — a discarded drag preview, an abandoned branch — cannot be written by mistake.

### `snapshots` — materialization caches

```sql
CREATE TABLE snapshots (
    op_hash  BLOB PRIMARY KEY NOT NULL,
    required INTEGER NOT NULL,  -- 1 = load-bearing, 0 = droppable cache
    encoding TEXT NOT NULL,
    data     BLOB NOT NULL
) WITHOUT ROWID;
```

`required = 0` is a pure optimization: delete it and the document still materializes, just more slowly.
`required = 1` means history behind this op has been pruned, and **deleting the row destroys data**.

A snapshot that will not decode is dropped with a load issue.
A *required* one that will not decode is a hard failure, because the history it stood in for is gone.

### `assets` — the name index

```sql
CREATE TABLE assets (
    asset_id TEXT PRIMARY KEY NOT NULL,  -- the same string a document property holds
    kind     TEXT NOT NULL,              -- load-bearing: what the asset is
    parts    BLOB NOT NULL,              -- an encoded vdoc value (array), see below
    meta     BLOB                        -- an encoded vdoc value (object), informational
) WITHOUT ROWID;
```

`parts` is an **ordered** array; each entry is an object:

| field | meaning |
|-------|---------|
| `hash` | 32 bytes, the blob's content hash |
| `format` | what the bytes are, e.g. `"png"` — selects a parser downstream |
| `name` | optional, **debug only** |

**Order is the contract; the name is for humans.**
Nothing may key behaviour on a part name, because the moment something does, renaming a part becomes a format change.

An asset with an empty `parts` array is legal, and means an asset that has metadata but no bytes.

### `blobs` and `blob_chunk` — the content store

```sql
CREATE TABLE blobs (
    id          INTEGER PRIMARY KEY,
    hash        BLOB NOT NULL UNIQUE,   -- 32-byte BLAKE3 over the DECODED bytes
    size        INTEGER NOT NULL,       -- decoded size
    stored_size INTEGER NOT NULL,       -- size as stored, i.e. after `encoding`
    chunk_count INTEGER NOT NULL,
    format      TEXT NOT NULL,
    encoding    TEXT NOT NULL           -- 'raw' in v1
);

CREATE TABLE blob_chunk (
    id          INTEGER PRIMARY KEY,
    blob_id     INTEGER NOT NULL,
    chunk_index INTEGER NOT NULL,
    data        BLOB NOT NULL,
    UNIQUE(blob_id, chunk_index),
    FOREIGN KEY(blob_id) REFERENCES blobs(id) ON DELETE CASCADE
);
```

**Blobs are shared.** Two assets naming the same content hash store the bytes once, whether by derivation or by coincidence.

`blobs` is a rowid table because incremental blob I/O addresses rows by rowid, which is how a chunk is read without materializing it in memory first.

**A torn write is visible rather than silent.**
There is no nullable data column that could be mistaken for "present": `chunk_count` and `stored_size` say what must be there, and a blob whose chunks do not add up is reported as incomplete.
That is why the payload is not simply a column on `blobs`.

Chunking also sidesteps SQLite's per-value size ceiling, and keeps a multi-gigabyte asset from being a single allocation.

`encoding` is the seam for compression.
`raw` is the only value v1 writes or reads; an unknown encoding is a load issue and the blob is skipped, never a failed open.
See [decisions.md](../../versioned-document/docs/decisions.md#blobs-ship-raw-only-with-the-encoding-seam-reserved).

### `workspace` — disposable UI state

```sql
CREATE TABLE workspace (
    key     TEXT PRIMARY KEY NOT NULL,
    version INTEGER NOT NULL,
    value   BLOB NOT NULL       -- an encoded vdoc value
) WITHOUT ROWID;
```

A separate table precisely so it can be `DELETE FROM`-ed in its entirety without a second thought.

- **Nothing here is load-bearing.** Discarding the whole table leaves a fully valid document.
- **A workspace write creates no op and moves no ref.** Moving a camera must not become edit history, and must not make the document look unsaved.
- `version` describes the shape of `value`; a reader that does not know a version **skips the entry and leaves it in place**.
- Only dirty keys are written, which is what keeps a newer build's keys unclobbered by an older one.
- Keys are slash-namespaced and coarse: one per cohesive settings struct, never one per field.

### `meta` — file-level facts

```sql
CREATE TABLE meta (
    key   TEXT PRIMARY KEY NOT NULL,
    value BLOB
) WITHOUT ROWID;
```

For facts about the file itself rather than the document in it — the writer's build, creation time, and whatever later needs a home that is neither history nor workspace.
Unknown keys are preserved untouched.

---

## Loading

1. Open the database, check `application_id` and `user_version`, ensure the schema.
2. Read `blobs` and `blob_chunk` **metadata only** — never a payload.
   `LENGTH()` on a blob column is answered from the row header, and there is one chunk row per large span, so this stays cheap on a multi-gigabyte file.
3. Read `assets`, filling in blob-side facts from step 2, and flagging assets whose blobs are missing or incomplete.
4. Read `ops`, decoding and **verifying every one**.
5. Read `refs`, `snapshots`, `workspace`, `meta`.

**Soft failures never block a load.**
A corrupt op is dropped and reported, landing on exactly the same downstream path as a pruned one — which is what makes that path get exercised rather than rotting.

Load issues are string-free: a kind plus the id it concerns.

| kind | meaning |
|------|---------|
| `op_decode_failed` | an op row's bytes would not decode |
| `op_hash_mismatch` | the bytes do not hash to the stored id — corruption or tampering |
| `missing_parent` | an op names a parent not in the file; informational, and normal after pruning |
| `missing_snapshot` | a snapshot row would not decode |
| `asset_blob_missing` | an asset names a content hash with no blob row |
| `asset_blob_incomplete` | the blob row exists but its chunks do not all |
| `unknown_encoding` | a blob names an encoding this build does not have |
| `unknown_table` | a table this build does not know; ignored |

The hard failures, by contrast, stop the open: not a SQLite database, unreadable, a `user_version` from the future, or a *required* snapshot that will not decode.

---

## Publishing

A caller asks to set refs, and to store assets and blobs.
**Ops are not listed** — the store derives them from the refs by reachability.
Assets and blobs must be listed, because the store cannot know about bytes sitting in memory.

The store computes `reachable − already durable`, writes everything in **one transaction**, and only then considers the refs moved.

- **Idempotent.** Publishing the same content twice is a no-op; ops and blobs are content-addressed and inserted by ignore-on-conflict.
- **A blob may be uploaded with no data**, meaning "you already have this", so nothing is read back just to be rewritten.
  Combined with a hash naming no stored blob, that is a publish error.
- **A failure latches.** The first one is kept and surfaced immediately, so a failing autosave is visible long before close.
  A *workspace* flush failure is deliberately not latched: losing a camera position is not the data loss that latch exists to report.

Closing flushes pending workspace writes, drains accepted publishes, rejects new ones, and severs the blob source — after which loading a blob fails cleanly rather than hanging on a dead handle.

---

## Reclaiming space

Blobs are reachable **from the asset index only**, never from ops.

So reclamation is one mark-and-sweep: mark every blob named by an asset, delete the rest, and `blob_chunk` follows by cascade.
An asset remap may legitimately orphan blobs, which is exactly the case this handles.

History pruning is separate and independent: attach a snapshot to an op, mark it `required`, then delete the ops behind it, leaving skeletons where the DAG still needs a position.
