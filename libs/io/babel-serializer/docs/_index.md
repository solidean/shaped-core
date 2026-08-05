# babel-serializer docs

Documentation hub for babel-serializer — serialization / deserialization of various formats (namespace `babel`).
Start at the [readme](../readme.md) for the overview and the source map, and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

## Topics

- [coding-guidelines.md](coding-guidelines.md) — the babel-specific rules, and the owner of every one of them.
  The two layers, read-once-then-query, what a reader takes as input, backends that may be absent, the writer convention, and the import-issue list.
- [structure.md](structure.md) — the format roadmap: what is `[done]` vs `[planned]`, per format.
- [lower-library-gaps.md](lower-library-gaps.md) — capabilities babel wants from clean-core / typed-geometry that do not exist yet, what it hand-rolls instead, and why.

Each format's own design lives in its header under [src/babel-serializer/](../src/babel-serializer/), which is where the roadmap and the cheat-sheet both point.
Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
