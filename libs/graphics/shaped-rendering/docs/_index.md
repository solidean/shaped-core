# shaped-rendering docs

Documentation hub for shaped-rendering. For the library overview start at the
[readme](../readme.md). For the whole graphics family see
[docs/graphics.md](../../../../docs/graphics.md); for repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

shaped-rendering is built on [shaped-graphics](../../shaped-graphics/readme.md) and
[shaped-shader-library](../../shaped-shader-library/readme.md); it hosts the concrete render routines
and the SDL3-backed window abstraction (`sr::window_system` / `sr::window`).
The render-routine *framework* lives in shaped-graphics (see
[its render-routines doc](../../shaped-graphics/docs/render-routines.md)).

## Topics

- [imgui](imgui.md) — the Dear ImGui renderer: how to drive it, why it is three types, and the 1.92
  texture protocol a backend has to implement.
- [render-routines](render-routines.md) — sr-side overview: writing a concrete routine, with a link to
  the framework doc in shaped-graphics.
- [structure](structure.md) — the intended module roadmap with status tags, including windowing.
- [coding-guidelines](coding-guidelines.md) — sr-specific conventions on top of the repo-wide ones: the SDL quarantine, and why the window API is always present while only its backend is optional.
- [TODO](TODO.md) — running list of known follow-ups.

## Conventions

- Namespace `sr`; depends on shaped-graphics + shaped-shader-library (and transitively typed-geometry
  + clean-core).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  sr-specific [coding-guidelines](coding-guidelines.md). `.clang-format` is authoritative for
  formatting.
