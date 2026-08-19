# versioned-document

Structured documents that are versioned, mergeable and verifiable.
Namespace `vdoc`. Depends on **clean-core** and nothing else.

A document is entities holding components holding properties.
The source of truth is not that document, but an immutable content-addressed DAG of **ops**; everything a reader sees is materialized from it.

```cpp
auto const head = graph.add(vdoc::op_builder{}
                                .set_parents(previous)
                                .set_raw(path, vdoc::value::of(3))
                                .build(graph));

auto const raw = graph.materialize(head);          // schema-agnostic
auto const doc = vdoc::parse(raw, policy, report); // typed, immutable, queryable
```

The library ships **zero components**.
What a `transform` or a `material` is belongs to the application; `vdoc` owns the storage model, the merge semantics and the interpretation machinery, and nothing above them.

Headers are included by their full path from `src/`, e.g. `#include <versioned-document/op_graph.hh>`.

The library is complete: values, ids, ops, the DAG, snapshots and pruning, the typed layer, recovery from an untrusted peer, and **layering**.
Layering composes several independent histories into one document, a higher layer replacing a lower one per property path — see [the concept](./docs/concepts/layering.md).
Persistence is [versioned-document-file](../versioned-document-file/readme.md), one library up.
The [concept docs](./docs/_index.md#concepts) are the design, one file per concept.

## Design at a glance

Four layers, kept strictly apart.
The [concept docs](./docs/_index.md#concepts) own all of it.

- **Ops** — an immutable, content-addressed DAG, where zero parents starts a document, one extends it and several merge.
  Canonicalization is the producer's job, so verifying a stored op is a plain hash of the bytes as stored, never a re-serialization.
- **The raw document** — `entity -> component -> property -> set of writes`, materialized from one or more heads, with no idea what any of it means.
  A property normally has one value; concurrent writers that do not dominate each other leave it with several.
- **The typed document** — interpretation: a registry of the application's component types, schema versions, deletion by convention, a conflict policy in and a diagnostics report out.
  Parsing never refuses: whatever this build cannot understand becomes a diagnostic while the rest of the document loads.
- **Persistence** — not here, but in [versioned-document-file](../versioned-document-file/readme.md), which stores a document, its assets and its blobs in a single `.vdoc` file.

Three properties are worth knowing before reading anything else:

- **The typed document is immutable** — there is no `set` on it.
  Edits build an op and re-materialize, which is what makes a snapshot safe to hold across threads for as long as you like.
- **Values are bytes.** A property value is a canonically-encoded binary value, so equality, hashing and "did these two writers agree" are all byte comparisons.
- **Two applications can share a document while each understands only its own half of it.**
  Not just old-versus-new builds — see [docs/compatibility.md](docs/compatibility.md).

## File organization

Source lives in `src/versioned-document/`.
`fwd.hh` doubles as the index of every name the library exposes.

| Area | What is in it |
|--------------|--------------------|
| (root)       | `fwd.hh` — forward declarations and vocabulary aliases |
| values       | `value` / `value_view` / `value_builder` — the binary value codec |
| identity     | `entity_id` / `component_type_id` / `property_id` / `property_path` — interned, distinct id types |
| ops          | `op` / `op_id` / `op_builder` / `op_graph` — the DAG and its materialization |
| raw          | `raw_document` and the three levels below it |
| snapshots    | `snapshot_document` / `snapshot_cache` — materializations cached against an op |
| recovery     | `received_op` and `integrate` — history taken from a peer, verified by recomputation |
| typed        | `component_registry` / `parse_policy` / `parse_report` / `document` |

## Building & testing

Build and test through the repo driver — never run a `*-test` binary directly:

```bash
uv run dev.py build -t versioned-document
uv run dev.py test versioned-document-test
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [docs/](./docs/_index.md) — the documentation hub, and the index of the concept docs that are the design.
- [docs/compatibility.md](docs/compatibility.md) — what a document guarantees across builds and across applications, and what an application owes in return.
- [docs/decisions.md](docs/decisions.md) — every settled decision, its reasoning, and what would reopen it.
- [cheat-sheet.md](cheat-sheet.md) — the API at a glance.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows.
