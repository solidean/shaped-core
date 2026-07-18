# shaped-rendering docs

Documentation hub for shaped-rendering. For the library overview start at the
[readme](../readme.md). For the whole graphics family see
[docs/graphics.md](../../../../docs/graphics.md); for repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

shaped-rendering is built on [shaped-graphics](../../shaped-graphics/readme.md) and
[shaped-shader-library](../../shaped-shader-library/readme.md); its first feature is the render-routine
framework.

## Topics

- [render-routines](render-routines.md) — the render-routine framework (routine / package / library),
  end to end.
- [structure](structure.md) — the intended module roadmap with status tags.
- [coding-guidelines](coding-guidelines.md) — sr-specific conventions (thin for now) on top of
  the repo-wide ones.
- [TODO](TODO.md) — running list of known follow-ups.

## Conventions

- Namespace `sr`; depends on shaped-graphics + shaped-shader-library (and transitively typed-geometry
  + clean-core).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  sr-specific [coding-guidelines](coding-guidelines.md). `.clang-format` is authoritative for
  formatting.
