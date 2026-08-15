# versioned-document docs

Documentation hub for versioned-document — structured documents that are versioned, mergeable and verifiable (namespace `vdoc`).
Start at the [readme](../readme.md) for the overview, and the [cheat-sheet](../cheat-sheet.md) for the planned API at a glance.

The concept is complete and the **value codec** is built; the layers above it are not.

## Topics

- [concept.md](concept.md) — the design, end to end, and the authority on what the library is and why it is shaped this way.
  The four layers, the value codec, content addressing, the multi-value model, interpretation, assets, and what deliberately lives outside a document.
- [decisions.md](decisions.md) — every settled decision, its reasoning, and what would reopen it.
  Read this before proposing a change that contradicts one.
- [structure.md](structure.md) — the roadmap: what is `[done]` versus `[planned]`, and which milestone lands each piece.
- [todo/](todo/_index.md) — the milestones, in execution order, with acceptance criteria.

The persistence layer has its own hub: [versioned-document-file](../../versioned-document-file/docs/_index.md).
Its [format.md](../../versioned-document-file/docs/format.md) specifies the on-disk shape.
Its milestones live here, in [todo/](todo/_index.md), because the two libraries are built to one plan.

Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
