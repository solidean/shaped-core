# The `.vdoc` file format

The on-disk specification.
A `.vdoc` file is a single SQLite database holding a document's whole history, the assets a user embedded in it, and the disposable UI state that goes with it.

The model it stores is [versioned-document](../../versioned-document/docs/_index.md#concepts)'s; read that first.
This document is the authority on the bytes.

Everything specified here is implemented.
The schema and the load path, publishing, the workspace, the content store — assets, blobs, the encoding seam and reclamation — snapshots with history pruning, and recovery from an untrusted peer.

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
- **Unknown columns are ignored**, reported, and preserved: a rewrite must never drop a column it did not understand.
  Preservation is structural rather than a step.
  Every statement names its own columns, there is no `SELECT *`, and no rewrite or table rebuild exists — so a column this build does not know cannot be addressed by anything on either path.

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

**The `assignments` blob is opaque to this format.**
Its leading tag names an encoding vdoc owns, and the only thing that ever reads inside it is `vdoc::try_decode_op`.
So a new assignment encoding is not a change to the `.vdoc` format: the column stores whatever bytes it is handed, and this file's version does not move with vdoc's tag set.

Both may be `NULL`, which is a **skeleton op**: a pruned parent, kept for its position in the DAG.
A skeleton is unverifiable by construction and must be reported as such, never as a hash mismatch.

The payload columns move in exactly two directions, and no others: non-NULL → NULL when a prune empties them, and NULL → non-NULL when a recovery fills them back in.
A row that already has bytes is never rewritten, because publishing is append-only and the fill is scoped to rows whose payload is NULL.

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
    op_hash      BLOB PRIMARY KEY NOT NULL,
    required     INTEGER NOT NULL,  -- 1 = load-bearing, 0 = droppable cache
    encoding     TEXT NOT NULL,     -- the payload codec: `raw` today
    decoded_size INTEGER NOT NULL,
    stored_size  INTEGER NOT NULL,
    chunk_count  INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE snapshot_chunk (
    op_hash     BLOB NOT NULL,
    chunk_index INTEGER NOT NULL,
    data        BLOB NOT NULL,
    PRIMARY KEY (op_hash, chunk_index),
    FOREIGN KEY (op_hash) REFERENCES snapshots(op_hash) ON DELETE CASCADE
) WITHOUT ROWID;
```

`required = 0` is a pure optimization: delete it and the document still materializes, just more slowly.
`required = 1` means history behind this op has been pruned, and **deleting the row destroys data**.

A snapshot that will not decode is dropped with a load issue.
A *required* one that will not decode is a hard failure, because the history it stood in for is gone.

**The payload is chunked rather than inline**, because SQLite caps a single value near a gigabyte and a snapshot of a large document goes past that.
`chunk_count` and `stored_size` say what must be there, so a snapshot whose chunks do not add up is visibly incomplete rather than silently short.
The bytes cascade off the snapshot row, so they die with it and nothing else can reach them.
That is deliberately unlike a blob, whose lifetime a reclamation decides.

`encoding` names the **payload codec**, the same table blobs use, so compression will arrive for both at once.

### The `snapshot-v1` payload

A snapshot holds the surviving writers of every property, and nothing else.
It is a materialized document rather than a resumable sweep state.
[decisions.md](../../versioned-document/docs/decisions.md#a-snapshot-stores-surviving-only-and-its-validity-is-decided-at-use) argues why.

All integers are u32 little-endian, matching the value codec's own length prefixes.
Names are the canonical id bytes, exactly what an id commits to.

| field | meaning |
|-------|---------|
| `writer_count`, then that many 32-byte ids | the writer table, ascending by canonical bytes |
| three name tables | entity, component and property names, each a count then `len`-prefixed bytes, ascending |
| `entity_count` | then, per entity: a name index and a component count |
| per component | a name index and a property count |
| per property | a name index and a writer count |
| per writer | a writer index, the value's byte length, then the value verbatim |

**Four intern tables, and they are what makes this affordable.**
A document of a few million properties would otherwise spend 32 bytes per property on writer ids alone, and the number of *distinct* writer ops is far smaller, since one op writes many paths.

**Every value carries its own extent.**
`vdoc::try_decode` rejects trailing bytes, so a value can only be validated against a slice already known to be exactly it — the same reason an assignment carries one.

The encoding is **canonical**, and the decoder enforces it: every table and every level ascending and deduplicated.
Two builds computing the same snapshot produce byte-identical rows.

### `assets` — the name index

```sql
CREATE TABLE assets (
    asset_id TEXT PRIMARY KEY NOT NULL,  -- the same string a document property holds
    kind     TEXT NOT NULL,              -- load-bearing: what the asset is
    parts    BLOB NOT NULL,              -- an encoded vdoc value (array), see below
    meta     BLOB,                       -- an encoded vdoc value (object), informational
    deps     BLOB                        -- an encoded vdoc value (array of asset id strings)
) WITHOUT ROWID;
```

`parts` is an **ordered** array; each entry is an object:

| field | meaning |
|-------|---------|
| `hash` | 32 bytes, the blob's content hash |
| `format` | what the bytes are, e.g. `"png"` — selects a parser downstream |
| `name` | optional, **debug only** |

**The name is the contract, and position within a name disambiguates.**
A part is addressed by `(name, index)`, where the index counts only the parts sharing that name.
Whole-list position carries nothing, so reordering an asset's parts changes no behaviour.

`name` is **absent when the part is `$main`**, the reserved default, and read back as `$main` — so the ceremony-free single-part asset stores no name at all.
An empty name is written explicitly, since it has to stay distinguishable from that default, and is reported at load.
`$` is reserved: an application must not invent its own `$`-name.

Duplicate names are reported and **kept**, because discarding somebody's part is not a loader's decision.
A lookup that reports ambiguity would also have nothing to report if the loader had already resolved it.

An asset with an empty `parts` array is legal, and means an asset that has metadata but no bytes.

**`deps` is declared by the application and never interpreted here.**
The store never parses a blob, so it cannot discover a dependency on its own; an application that wants a precise sweep declares one, and one that declares nothing gets a sweep that collects more.
NULL means no declared dependencies.

An id naming nothing in this file is legal and **silently skipped**, never a load issue.
A file is one asset source among many, so a dependency may name a built-in, procedural, remote or otherwise externally-stored asset.
Order carries no meaning, duplicates are harmless, and cycles are ordinary — the closure walk is a flood fill whose visited set terminates them.
An entry that is not a string is dropped, and a whole `deps` blob that will not decode is reported and read as absent.
Neither drops the asset: a dependency list is advisory, so an unreadable one costs a later sweep precision rather than costing the reader the asset.

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
4. Read `ops`, decoding and **verifying every one**, then check each op's parents against what the file actually held.
5. Read `refs`, `snapshots`, `workspace`, `meta`.
   A snapshot is **decoded** into the materialization cache, and a `required` one is pinned there so shedding cache memory cannot destroy it.
   The file records what it has — the op, the flag, the encoding, the decoded size — and never keeps the payload resident.

**Soft failures never block a load.**
A corrupt op is dropped and reported, landing on exactly the same downstream path as a pruned one — which is what makes that path get exercised rather than rotting.

Load issues are string-free: a kind plus the id it concerns.

| kind | meaning |
|------|---------|
| `op_decode_failed` | an op row's bytes would not decode |
| `op_hash_mismatch` | the bytes do not hash to the stored id — corruption or tampering |
| `missing_parent` | an op names a parent not in the file; informational, and normal after pruning |
| `missing_snapshot` | a snapshot's payload would not decode, or its chunks do not add up; names the `op`, except where the row's key was not an op id at all |
| `dangling_ref` | a ref names an op not in the file, usually one this load dropped; the ref is kept anyway |
| `asset_decode_failed` | an asset's `parts` or `meta` blob would not decode, which drops the asset; or its `deps` blob would not, which does not |
| `asset_part_unnamed` | an asset carries a part with an empty name, which nothing can address |
| `asset_duplicate_part_name` | several parts share one name; all are kept, and a singular lookup reports it as ambiguous |
| `asset_blob_missing` | an asset names a content hash with no blob row |
| `asset_blob_incomplete` | the blob row exists but its chunks do not all add up |
| `unknown_encoding` | a blob or a snapshot names an encoding this build does not have |
| `workspace_decode_failed` | a workspace row's value would not decode; the row is left in place |
| `unknown_table` | a table this build does not know; ignored, and left untouched |
| `unknown_column` | a column this build does not know on a table it does know; ignored, and preserved |

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

Reclamation therefore has two levels, and the caller supplies the roots of the first.

1. **The asset closure.** Starting from the root asset ids the caller names, flood-fill through each asset's declared `deps`, and delete every asset outside the result.
   The visited set is what makes a cycle terminate, and an id naming nothing in this file is skipped.
2. **The blob sweep.** Mark every blob named by a *retained* asset, delete the rest, and `blob_chunk` follows by cascade.
   "The rest" is the blobs this build actually read at load, so a blob under an unknown `encoding` is never collected — a build does not delete rows it cannot read.

Both happen in one transaction, so a file never holds an asset whose blob was already collected.

**Marking is still from the asset index and only from there** — the change is that the index is first narrowed by a caller-supplied root set.
That is what lets an application name what it wants to keep instead of resolving the closure itself.
An asset remap may legitimately orphan blobs, which is exactly the case this handles.

Unmapping a single name is a separate, cheaper act: it rides a publish, is retroactive exactly like a remap, and collects no bytes.

History pruning is separate and independent, and has its own section below.

---

## Pruning history

Attach a `required` snapshot to an op, then empty every op behind it — leaving a **skeleton**: the row, its id and its parents, with both payload columns NULL.

Deleting the rows instead would sever ancestry through them.
Two writes that were ordered would read as concurrent, and materialization would manufacture a multi-value nobody authored.
That is why the position survives even where the content does not.

**How far a document may prune is bounded, and the bound is not obvious.**
`prune` refuses unless **every ref descends from the prune point**, and names the ref that blocked it.

A required snapshot carries no superseded set, which is sound only while nothing can present a writer from behind it.
Ops behind the boundary are skeletons and carry no writers — but a ref that forked *before* the boundary keeps its own ancestors, because they are history it still needs.
That branch does still offer writers the emptied ops superseded, and merging the two would fabricate a multi-value.
Replaying instead is no escape either: the ops that would have suppressed it are skeletons by then, so the replay is lossy rather than merely slow.

So the boundary a document may prune to is the **oldest op every ref still descends from**.

The write is one transaction, snapshots first and skeletons second, through its own store hook — a publish only ever appends and is idempotent by content addressing, while this destroys.

**The trade-off, stated where a user can see it:** less storage and a faster load, against losing deep history and shortening the range over which two replicas can still synchronize.
Pruning is always optional, and never automatic.

After a prune, materializing **without** the snapshot is lossy by construction, since the emptied ops carry nothing.
That is what `required` means, and why deleting such a row destroys data.

---

## Recovering history from a peer

An op id recursively commits to everything behind it, so a replica missing history can accept that history from anyone at all.
It recomputes the hashes over the bytes as received and checks them against the ids it already expected; nothing is re-serialized, and the sender is trusted at no point.

The batch is a **set**.
Every op is verified before any of it is stored, so a partial or hostile batch is refused naming the op — and leaves the file byte-identical, because a refusal never opens a transaction.

The write is one transaction through its own store hook, ops first and snapshot demotions second.
Each op is inserted and then filled: the insert conflicts away where the row is already there, and the fill only touches NULL payload columns.
So the write never has to classify an op correctly in order to be correct.

**A recovery can retire a prune boundary, and that is the only thing that moves `required` back to 0.**
Once every op behind a required snapshot has its payload again, replaying reproduces exactly what the snapshot holds, so the row stops being load-bearing and is demoted to a droppable cache.
The demotion moves the flag alone — the chunks are already correct, and re-encoding a payload that can run to gigabytes to move one bit would be the expensive way to change nothing.

Until that happens the boundary still binds, from the other direction: **a batch introducing an op that forks below a still-required snapshot is refused**.
That op would present a writer the emptied ops superseded and nothing can suppress — the same fabricated multi-value pruning refuses to create.
Sending the rest of that snapshot's ancestry in the same batch is what makes such a batch acceptable, which is why this is a boundary rather than a ban.

The demotion is written **after** the fills, and the order is load-bearing.
A crash between them leaves a snapshot still marked required over history that is already back, which costs one pinned cache entry.
The reverse would leave a droppable snapshot standing over history that is still gone, which is data loss.
