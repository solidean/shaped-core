# versioned-document-file — Cheat Sheet

The `.vdoc` save format: one SQLite file holding a document's history, its embedded assets, and its workspace state.
Namespace `vdoc::file`. Depends on versioned-document and babel-serializer.

> The store, the loader, publishing, the workspace and the whole content store are real.
> Snapshots are **carried but not yet decoded** — entries below say which.
> The on-disk shape is fully specified in [docs/format.md](docs/format.md).

## Opening and reading

```cpp
auto [file, loaded] = vdoc::file::store::open("project.vdoc");  // returns AT ONCE, having touched no disk
auto mem = vdoc::file::store::create_in_memory();               // an unsaved new document
vdoc::file::store::is_file_storage_available();                  // false where SQLite was not compiled in

// `loaded` is cc::shared_async<cc::unit>: ready once the load finished, and a HARD failure rides its error.
file->ops();          // vdoc::op_graph const& — loaded and verified in full
file->refs();         // cc::map<cc::string, op_id> — kept verbatim, dangling ones included
file->snapshots();    // cc::map<op_id, snapshot_entry> — OPAQUE here; milestone 6 decodes them
file->assets();       // cc::map<cc::string, asset_record>
file->meta();         // cc::map<cc::string, vdoc::value> — file-level facts, not document ones
file->report();       // load_report: the soft failures

file->add_op(op);     // a newly built op; publishable only once a ref reaches it
```

The op DAG loads **eagerly and completely**; blobs load **lazily**.
A hard failure — not a database, not ours, a format version from the future, an unreadable required snapshot — fails the load.
Everything else is a load issue and the file still opens.

**`open` hands back the store synchronously and reports the load separately.**
That is not a convenience: the caller must own the store from the first instant, because an actor holding the last reference would tear itself down on its own thread.

## Publishing

```cpp
file->publish({
    .refs            = {{"main", head}},   // ops are DERIVED from these by reachability
    .assets          = {...},              // asset_record, upserted
    .blobs           = {...},              // blob_upload, deduplicated by hash
    .removed_assets  = {"old/name"},       // unmapped AFTER the upserts; collects no bytes
});                                        // -> cc::shared_async<publish_result>{ops_written, blobs_written}
```

- **Ops are never listed.** An op no ref can reach cannot be published by mistake, even by a caller who wanted to.
- **Idempotent.** Content-addressed inserts; a second publish of the same thing returns `{0, 0}` and writes nothing.
- **Fire and forget.** Hold the returned async and wait at save or close; a failure also latches.

```cpp
file->is_saved(head);      // would publishing `head` be a no-op? drives "nothing to save"
file->sticky_error();      // cc::any_error const* — the FIRST failure, so a failing autosave surfaces immediately
file->close();             // flush workspace, drain publishes, reject new ones, sever the blob source
```

`is_saved` means *queued*, not *committed* — refs update at enqueue time.
A publish that later failed **un-claims its ops**, so `is_saved` goes back to false and a retry writes them again.

## Workspace

```cpp
file->set_workspace("viewport/camera", {.version = 1, .value = v});  // no I/O, safe every frame
file->try_get_workspace("viewport/camera", 1);                       // empty unless stored under THIS version
file->flush_workspace();                                             // writes only dirty keys
```

- **Never creates an op, never moves a ref, never affects `is_saved`.** Moving a camera must not look like an edit.
- **The caller names the version it can handle** — the store cannot know an application's versions.
  A row under any other version reads as absent and stays in the table.
- Only dirty keys are written, so a newer build's keys survive an older build touching the file.
- A workspace failure is deliberately **not** latched into the sticky error.
- `close()` flushes, so state cannot be lost by forgetting to.

## Diagnostics

```cpp
file->report().is_empty();
file->report().contains(vdoc::file::load_issue_kind::op_hash_mismatch);
file->report().count_of(kind);
file->report().find_first(kind);   // load_issue const* — the op, asset or table it concerns
```

String-free: a kind plus the id it concerns, so a caller localizes the message and a test asserts on the kind exactly.
The fourteen kinds are [format.md](docs/format.md#loading)'s table.

## Assets and blobs

```cpp
struct asset_record
{
    cc::string             asset_id;      // the same string a document property holds
    cc::string             kind;          // load-bearing
    cc::vector<asset_part> parts;         // addressed by (name, index); stored order is kept verbatim
    vdoc::value            meta;          // informational
    cc::vector<cc::string> dependencies;  // DECLARED asset ids; uninterpreted, may name assets outside this file
    bool                   is_resolvable; // false where a part's blob is missing or incomplete
};

struct asset_part
{
    vdoc::file::blob_hash hash;        // 32-byte BLAKE3 over the decoded bytes
    cc::string            format;      // "png", "mesh", ... — selects a parser downstream
    cc::string            name = "$main";  // THE CONTRACT; `$` is reserved
};

blob_upload::of(decoded, "png");       // -> cc::result<blob_upload>; hashes and encodes in one step
```

Reaching parts, all on the record so they share one snapshot:

```cpp
record.main_part();               // -> cc::result<asset_part const*, part_lookup_error>; exactly one $main
record.try_find_part("preview");  // -> same; not_found and ambiguous are DISTINCT errors
record.part_at("lod", 1);         // -> cc::optional<asset_part const*>; explicit index, absent is ordinary
record.parts_named("lod");        // -> part_range, declaration order, random access + iteration
record.main_parts();              // -> part_range; the escape hatch after `ambiguous`
```

Prefer `blob_upload::of` to filling the struct by hand.
Hand-filling has to get `decoded_size` right — it is REQUIRED under any encoding but `raw`, and a publish that omits it fails.

```cpp
auto source = file->make_blob_source();      // a NEW source each call; keeps the storage alive
source->load(hash);                          // async, DECODED bytes; severed after close()
source->load_range(hash, offset, size);      // a byte range; size < 0 means to the end

file->resolve_asset("meshes/wall");          // -> optional<asset_resolution>{record (a COPY), blobs}
file->pump();                                // an in-memory store's fetches complete HERE
```

```cpp
file->reclaim(roots);   // -> cc::shared_async<reclaim_result>{assets_removed, blobs_removed}
```

Keeps the closure of `roots` under each asset's `dependencies`, deletes the rest, then marks blobs from what survived and sweeps — all in one transaction.

Gotchas:

- **Blobs are shared across assets.** Identical bytes are stored once, by derivation or by coincidence.
- **Parts load independently** — fetch a header or one level of detail without the rest, via `load_range`.
- **Part names are the contract**, and the index counts only within a name.
  Whole-list position carries nothing.
- **A singular lookup errors rather than picking one.** Several parts under one name is `ambiguous`, never "the first".
- **Duplicates are kept at load**, so the report names them at open and the lookup names them at use.
- **Reclamation marks from the asset index**, narrowed by the root set.
  Blobs are never reachable from ops, and an asset remap may legitimately orphan blobs.
- **A dependency naming nothing in this file is normal**, not an issue: a file is one asset source among many.
  Cycles are ordinary too.
- **`load` never blocks and never re-enters its caller.** It may be called with a caller's lock held, so the answer always arrives later — pump your loop.

## Testing against a store

```cpp
auto image = std::make_shared<vdoc::file::memory_image>();
auto s = vdoc::file::store::create_in_memory(image);   // the image OUTLIVES the store
s->close();
auto reopened = vdoc::file::store::create_in_memory(image);  // re-runs the load, verification and issues included
```

That is what makes the in-memory arm an oracle rather than a shortcut, and it is how the conformance suite runs one set of tests over both implementations.
`memory_image::writes_fail` injects a storage failure, so both arms can fail the same way.

## Patterns & gotchas

- **Only history is immutable.** Blobs are content-addressed, but the **name → asset mapping is mutable and remapping is retroactive, on purpose**.
  So **op ids do not commit to asset content** — a document is reproducible only relative to an asset resolution.
- **A store is a seam.** The loaded state is plain members filled once at load; only keeping it in sync with storage is virtual, and that is five hooks.
- **One actor owns the connection**, and only the SQLite arm has one.
  No caller blocks on storage, and no lock is held across a read.
- **One thread owns a store.** The API is non-blocking because storage work runs on an actor, not because several threads may call in.
- **Soft failures never block a load.** A corrupt op is dropped and reported, on the same path as a pruned one.
- **A skeleton op is unverifiable, not corrupt.** A pruned parent has no bytes to hash, and must never be reported as a mismatch.
- **`encoding` is a reserved seam.** `raw` is all v1 writes; an unknown encoding skips the blob with an issue rather than failing the open.
- **Unknown tables and columns survive** an open-modify-save cycle, because every statement names its own columns and no rewrite exists.
