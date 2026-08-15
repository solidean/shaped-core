# Milestone 4 — The file

**Status: done.**
Landed as `diagnostics`, `assets`, `workspace`, `rows`, `memory_image`, `store`, and the `impl/` half.
That half is the shared loader and publisher, the in-memory arm, and the SQLite arm with its schema, its I/O and its actor.
The conformance suite runs on both implementations, under both `SC_THREADS` settings.

Four departures from what is written below, each argued where it is recorded:

- **`open()` hands the caller the store synchronously**, and reports the load through a separate async.
  An actor holding the last reference to the store would destroy it — and with it the actor — on the actor's own thread, which is a join against itself.
  Nothing touches the disk on the calling thread either way, so "no caller-visible API blocks on storage" still holds.
- **`.shaped-lint.yml` is no longer empty**: it blesses `<memory>` for the polymorphic store handle.
  The clause's intent survives — no sqlite type appears anywhere, and babel::sqlite is still extended rather than bypassed.
  The letter of it does not, so the acceptance below is corrected rather than quietly ignored.
- **A snapshot is opaque here.** It round-trips byte for byte, and `encoding` is the seam milestone 6's decoder attaches to.
- **The store gained three things neither this file nor format.md named**: `add_op`, `try_get_workspace(key, version)`, and counts on `publish_result`.

**Goal.** `versioned-document-file`: the `.vdoc` SQLite format, loading and verification, refs, publishing, the workspace side table, and the actor that owns the connection.

**Why here.** The model is complete and testable without persistence, which is exactly why persistence comes after it.
The store is built against a model already known to be correct, and its conformance suite compares two implementations rather than one implementation against its own assumptions.

The specification is [format.md](../../../versioned-document-file/docs/format.md), which is the authority on every byte.
Depends on milestones 0 (item C), 2 and 3.

Assets and blobs are milestone 5.
Their **tables and their load path** are built here — a file written today must be readable by the milestone-5 code without a format change — but nothing populates them yet.

---

## Work items

### 1. The schema

Exactly as [format.md](../../../versioned-document-file/docs/format.md) specifies: `meta`, `ops`, `refs`, `snapshots`, `assets`, `blobs`, `blob_chunk`, `workspace`.

- `PRAGMA application_id = 0x56444F43`, `PRAGMA user_version = 1`, `PRAGMA foreign_keys = ON`.
- Create what is missing; never rewrite what is there.
- A `user_version` from the future is a **hard failure**. Guessing at a shape this build might misread is worse than refusing.
- An old shape that cannot be read forward reports **what is actually wrong**, at the point it is detected.
  A `CREATE TABLE IF NOT EXISTS` over an incompatible old table leaves it in place and every later statement fails obscurely — say the real thing instead.
- Unknown tables and unknown columns are ignored, reported, and **preserved**. A rewrite must never drop what it did not understand.

### 2. `store`, the seam

Two implementations, one interface, one conformance suite:

- **in-memory** — where an unsaved new document lives, and the oracle the suite compares against;
- **SQLite-backed** — the real file.

Loaded state is plain members filled once at load; only keeping the file open and in sync is virtual.
That split is what keeps the two implementations from drifting: they share everything except persistence.

Surface: `open(path)`, `create_in_memory()`, `ops()`, `refs()`, `snapshots()`, `assets()`, `report()`, `publish()`, `is_saved(head)`, `sticky_error()`, `close()`.

### 3. Loading

In the order [format.md](../../../versioned-document-file/docs/format.md#loading) gives, and **metadata before payloads**.
The blob scan reads row headers and never a payload, so opening a multi-gigabyte file stays cheap.

Every op is decoded through milestone 2's verifying decoder, and there is no other route.

**Soft failures never block a load.**
A corrupt op is dropped and reported, landing on the same downstream path as a pruned one — which is what keeps that path exercised rather than rotting until the day it is needed.

Hard failures, and only these: not a database, unreadable, a future `user_version`, a *required* snapshot that will not decode.

### 4. The actor

A `cc::threaded_actor` owns the database handle exclusively; results come back as push-based `cc::async` values.

- No caller ever blocks on storage, and no lock is ever held across a read.
- The actor is the only thing that touches the connection — make that structural, not a convention.
- Honour `CC_HAS_THREADS`: on a single-threaded build the actor runs on the calling thread, and every API stays present and behaves identically.
  **No declaration is ever compiled away.**

### 5. Publishing

A caller asks to set refs, and lists assets and blobs.
**Ops are not listed** — the store derives them from the refs by reachability.

That is a safety property, not a convenience: an op no ref can reach — an abandoned branch, a discarded drag preview — cannot be published by mistake.

- Compute `reachable − already durable`, write everything in **one transaction**, and only then consider the refs moved.
- Idempotent: content-addressed inserts, so publishing twice is a no-op.
- Fire-and-forget, with the async held and waited on at save or close.
- **The first failure latches** into a sticky error, so a failing autosave surfaces immediately rather than at close.
- Track the durable op set to derive deltas.
  It is a pure optimization, since publishing is idempotent, so it must never be *required* for correctness.

### 6. The workspace

Disposable UI state, in its own table so it can be deleted wholesale without a second thought.

- `set_workspace(key, value)` marks dirty, performs no I/O, and is safe to call every frame.
- `flush_workspace()` writes **only dirty keys**, which is what keeps a newer build's keys unclobbered.
- **Never creates an op, never moves a ref, never affects `is_saved`.** Moving a camera must not look like an edit, and must not make the document appear unsaved.
- A workspace failure is deliberately **not** latched, because losing a camera position is not the data loss the latch exists to report.
- `close()` flushes, so nothing is lost by forgetting to.
- The cadence belongs to the caller — this layer has no clock.

### 7. Closing

Flush the workspace, drain accepted publishes, reject new ones, close, and sever the blob source.
Afterwards, publishing fails fast and loading a blob completes with an error rather than hanging on a dead handle.

Make the flush impossible to skip: the public `close` is non-virtual and calls a protected hook, so no implementation can forget it.

## Tests

A `versioned-document-file-test` binary, with the **conformance suite as the centre of it**.
One set of tests, parametrized over both implementations, using nexus's invocable tests.

- **Round-trip**: build a document, publish, close, reopen, and materialize to the same bytes.
- **Reachability**: an op unreachable from any published ref is not written.
  Assert it by absence, on the reopened file.
- **Idempotence**: publish the same thing twice; the file is unchanged the second time.
- **Torn state**: a file with a corrupt op row opens, reports the right issue, and still materializes the ops that are fine.
- **Hash mismatch**: flip one stored byte; the op is dropped with `op_hash_mismatch` and the rest of the document loads.
- **Future `user_version`** fails the open, cleanly and specifically.
- **Unknown tables and columns** survive an open-modify-save cycle untouched — this is the forward-compatibility promise, so it is tested rather than asserted.
- **Workspace**: writing it never changes `is_saved`; an unknown version is skipped and left in place; only dirty keys are written; `close` flushes.
- **Sticky error**: a failing publish latches the first failure and it is readable immediately; a failing workspace flush does not latch.
- **Transactions**: an interrupted publish leaves the file byte-identical, with no partial op set.
- **Single-threaded build**: the whole suite passes with `SC_THREADS=OFF`, which is what proves the actor's fallback rather than assuming it.

## Acceptance

- Both store implementations pass one identical conformance suite.
- No sqlite type appears anywhere in this library, and [its `.shaped-lint.yml`](../../../versioned-document-file/.shaped-lint.yml) carries nothing but the named clean-core gap.
  An entry blessing `<sqlite3.h>` would still mean the babel wrapper was bypassed instead of extended, and there is none.
- No caller-visible API blocks on storage.
- A publish is atomic; there is no observable half-published state.
- A workspace write never makes a document look unsaved.
- The suite passes under both `SC_THREADS` settings.
