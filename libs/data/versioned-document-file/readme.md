# versioned-document-file

The `.vdoc` save format: one file holding a document's whole history, the assets a user embedded in it, and the UI state that goes with it.
Namespace `vdoc::file`.
Depends on **versioned-document** (the model) and **babel-serializer** (the SQLite engine, linked privately).

```cpp
// [planned] — the shape the API is being built toward, not something you can compile today.
auto file = vdoc::file::store::open("project.vdoc");

auto const& graph = file->ops();           // the whole DAG, loaded and verified
auto const  head  = file->refs()["main"];  // a named head

file->publish({.refs = {{"main", new_head}}});  // ops derived from the refs by reachability
```

One file is one shareable unit: send it to someone and they have the document, its history and its embedded content.
The op DAG loads eagerly and completely; blobs load lazily through a [`blob_source`](src/versioned-document-file/fwd.hh).

This library is at the **design stage** — nothing is implemented.
[docs/format.md](docs/format.md) specifies the on-disk shape, and the plan to build it lives with the model library, in [versioned-document/docs/todo/](../versioned-document/docs/todo/_index.md).

## The one thing to know first

**A `.vdoc` file holds three kinds of state, and only one of them is immutable.**

| | Immutable? | What that means |
|---|---|---|
| op DAG, refs, snapshots | yes | content-addressed, verified on load, append-only |
| asset index, blobs | blobs only | blobs are content-addressed and shared; the **name → asset mapping is mutable, and remapping is retroactive on purpose** |
| workspace | no | disposable UI state; deleting all of it leaves a fully valid document |

So **op hashes do not commit to asset content**, and a document is reproducible only relative to an asset resolution.
That is the escape hatch that makes the format usable for real content work rather than a museum of exact bytes.
[docs/format.md](docs/format.md) says why at length, and it must not be "fixed" by folding assets into the DAG.

## Design at a glance

- **A store is a seam, not a class hierarchy.** Two implementations satisfy it — SQLite-backed and in-memory — and one conformance suite runs against both.
  The in-memory one doubles as the oracle, and is where an unsaved new document lives, so "save as" is just publishing everything into a file.
- **Writes are a write-behind log.** Publishing appends ops reachable from the committed refs and never renames a file into place, so it is crash-safe without an "unsaved changes" dance.
- **One worker owns the connection.** A `cc::threaded_actor` holds the database exclusively and answers with push-based `cc::async` values.
  So no caller ever blocks on storage, and no lock is held across a read.
- **Assets are one source among many** — this file stores what a user embedded persistently.
  Built-in, procedural and downloaded assets resolve elsewhere entirely, and asset ids stay plain strings so they can.
- **The workspace is not the document.** Moving a camera writes workspace state, creates no op, moves no ref, and never makes the document look unsaved.

## File organization

Source lives in `src/versioned-document-file/`.
Only `fwd.hh` exists so far, and it doubles as the index of every name the library plans to expose.

| Planned area | What will be in it |
|--------------|--------------------|
| (root)       | `fwd.hh` — forward declarations and vocabulary aliases |
| store        | `store` — open, load, publish, close; plus the in-memory and SQLite implementations behind it |
| assets       | `asset_record` / `asset_part` / `blob_hash` / `blob_upload` / `blob_source` |
| workspace    | `workspace_value` / `workspace_entry` |
| diagnostics  | `load_issue_kind` / `load_issue` / `load_report` |

## Building & testing

```bash
uv run dev.py build -t versioned-document-file
```

There is no `versioned-document-file-test` yet; it arrives with [milestone 4](../versioned-document/docs/todo/milestone-4.md).
See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [docs/format.md](docs/format.md) — the on-disk specification: tables, columns, encodings, and the compatibility rules.
- [docs/_index.md](docs/_index.md) — this library's documentation hub.
- [cheat-sheet.md](cheat-sheet.md) — the planned API at a glance.
- [versioned-document/docs/concept.md](../versioned-document/docs/concept.md) — the model this persists.
