# shaped-viewer docs

Documentation hub for shaped-viewer. For the library overview start at the
[readme](../readme.md). For the whole graphics family see
[docs/graphics.md](../../../../docs/graphics.md); for repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

shaped-viewer is an **early-stage skeleton** built on
[shaped-rendering](../../shaped-rendering/readme.md).

## Topics

- [structure](structure.md) — the intended module roadmap with `[planned]` status.
- [coding-guidelines](coding-guidelines.md) — sv-specific conventions (thin for now) on top of
  the repo-wide ones.
- [TODO](TODO.md) — running list of known follow-ups.

## Conventions

- Namespace `sv`; depends on shaped-rendering (and transitively shaped-graphics, typed-geometry,
  clean-core).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  sv-specific [coding-guidelines](coding-guidelines.md). `.clang-format` is authoritative for
  formatting.
