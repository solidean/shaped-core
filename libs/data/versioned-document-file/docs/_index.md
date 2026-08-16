# versioned-document-file docs

Documentation hub for versioned-document-file — the `.vdoc` save format (namespace `vdoc::file`).
Start at the [readme](../readme.md) for the overview, and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

Repo-wide docs are at [docs/_index.md](../../../../docs/_index.md).

## Topics

- [format.md](format.md) — the on-disk specification, and the authority on the bytes.
  The three durability classes, the tables, the load procedure and its soft failures, publishing, and how space is reclaimed.
- [benchmarks/document-loop-benchmark.md](benchmarks/document-loop-benchmark.md) — what an ordinary open / edit / publish / close loop costs, stage by stage.
  Written to check the standing reservation about BLAKE3's cost, and it is where that reservation was tested rather than argued.

The model this persists is [versioned-document](../../versioned-document/docs/_index.md)'s:

- [the concept docs](../../versioned-document/docs/_index.md#concepts) — the design of the document model itself, one file per concept.
  [snapshots](../../versioned-document/docs/concepts/snapshots.md) and [pruning and recovery](../../versioned-document/docs/concepts/pruning-and-recovery.md) are the two this library carries most of.
- [decisions.md](../../versioned-document/docs/decisions.md) — the settled choices, including the ones this library depends on.
  The table split, the mutable asset mapping, and the reserved encoding seam.

Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
