# versioned-document-file docs

Documentation hub for versioned-document-file — the `.vdoc` save format (namespace `vdoc::file`).
Start at the [readme](../readme.md) for the overview, and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

The store, the loader, publishing, the workspace, the content store and snapshots with history pruning are implemented; recovery from an untrusted peer is what remains.

## Topics

- [format.md](format.md) — the on-disk specification, and the authority on the bytes.
  The three durability classes, the tables, the load procedure and its soft failures, publishing, and how space is reclaimed.
- [benchmarks/document-loop-benchmark.md](benchmarks/document-loop-benchmark.md) — what an ordinary open / edit / publish / close loop costs, stage by stage.
  Written to check the standing reservation about BLAKE3's cost, and it is where that reservation was tested rather than argued.

The model this persists is [versioned-document](../../versioned-document/docs/_index.md)'s:

- [concept.md](../../versioned-document/docs/concept.md) — the design of the document model itself.
- [decisions.md](../../versioned-document/docs/decisions.md) — the settled choices, including the three this library depends on.
  The table split, the mutable asset mapping, and the reserved encoding seam.
- [todo/](../../versioned-document/docs/todo/_index.md) — the milestones, of which this library's work is 4 through 6; 4 and 5 have landed.
  Both libraries are built to one plan.

Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
