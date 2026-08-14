# versioned-document-file — Cheat Sheet

The `.vdoc` save format: one SQLite file holding a document's history, its embedded assets, and its workspace state.
Namespace `vdoc::file`. Depends on versioned-document and babel-serializer.

> **Everything on this sheet is `[planned]`.**
> Nothing here compiles today — only `fwd.hh` exists.
> The on-disk shape is fully specified in [docs/format.md](docs/format.md); this sheet is the intended C++ surface over it.
> Entries move from `[planned]` to real as [milestones 4–6](../versioned-document/docs/todo/_index.md) land.

## Opening and reading — `[planned]`

```cpp
auto file = vdoc::file::store::open("project.vdoc");   // async; hard failures ride the error
auto mem  = vdoc::file::store::create_in_memory();     // an unsaved new document

file->ops();          // vdoc::op_graph const& — loaded and verified in full
file->refs();         // name -> op_id
file->snapshots();    // op_id -> raw_document
file->assets();       // asset_id string -> asset_record
file->report();       // load_report: the soft failures
```

The op DAG loads **eagerly and completely**; blobs load **lazily**.
A hard failure — not a database, unreadable, a format version from the future — fails the open.
Everything else is a load issue and the file still opens.

## Publishing — `[planned]`

```cpp
file->publish({
    .refs   = {{"main", head}},   // ops are DERIVED from these by reachability
    .assets = {...},
    .blobs  = {...},
});
```

- **Ops are never listed.** An op no ref can reach cannot be published by mistake.
- **Idempotent.** Content-addressed inserts; publishing the same thing twice is a no-op.
- **Fire and forget.** Hold the returned async and wait at save or close; a persistent failure also latches.

```cpp
file->is_saved(head);      // would publishing `head` be a no-op? drives "nothing to save"
file->sticky_error();      // the FIRST failure, so a failing autosave surfaces immediately
file->close();             // flush workspace, drain publishes, reject new ones, sever the blob source
```

`is_saved` means *queued*, not *committed* — refs update at enqueue time.
The sticky error is what catches a publish that later failed; wait on the async itself if you need committed.

## Workspace — `[planned]`

```cpp
file->set_workspace("viewport/camera", {.version = 1, .value = v});  // no I/O, safe every frame
file->flush_workspace();                                            // writes only dirty keys
```

- **Never creates an op, never moves a ref, never affects `is_saved`.** Moving a camera must not look like an edit.
- Only dirty keys are written, so a newer build's keys survive an older build touching the file.
- A workspace failure is deliberately **not** latched into the sticky error.
- `close()` flushes, so state cannot be lost by forgetting to.

## Assets and blobs — `[planned]`

```cpp
struct asset_record
{
    cc::string             asset_id;   // the same string a document property holds
    cc::string             kind;       // load-bearing
    cc::vector<asset_part> parts;      // ORDERED — order is the contract
    vdoc::value            meta;       // informational
};

struct asset_part
{
    vdoc::file::blob_hash hash;        // 32-byte BLAKE3 over the decoded bytes
    cc::string            format;      // "png", "mesh", ... — selects a parser downstream
    cc::string            name;        // DEBUG ONLY, never load-bearing
};
```

```cpp
auto source = file->blob_source();          // shared; keeps the storage alive
source->load(hash);                          // async, decoded bytes
```

Gotchas:

- **Blobs are shared across assets.** Identical bytes are stored once, by derivation or by coincidence.
- **Parts load independently** — fetch a header or one level of detail without the rest.
- **Part names are for humans.** Key behaviour on order, never on a name.
- **Reclamation marks from the asset index.** Blobs are never reachable from ops, and an asset remap may legitimately orphan blobs.

## Patterns & gotchas — `[planned]`

- **Only history is immutable.** Blobs are content-addressed, but the **name → asset mapping is mutable and remapping is retroactive, on purpose**.
  So **op ids do not commit to asset content** — a document is reproducible only relative to an asset resolution.
- **A store is a seam.** The in-memory and SQLite implementations pass one conformance suite, and the in-memory one is the oracle.
- **One actor owns the connection.** No caller blocks on storage, and no lock is held across a read.
- **Soft failures never block a load.** A corrupt op is dropped and reported, on the same path as a pruned one.
- **A skeleton op is unverifiable, not corrupt.** A pruned parent has no bytes to hash, and must never be reported as a mismatch.
- **`encoding` is a reserved seam.** `raw` is all v1 writes; an unknown encoding skips the blob with an issue rather than failing the open.
