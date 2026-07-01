# shaped-viewer

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API,
built on [shaped-rendering](../shaped-rendering/readme.md). Namespace `sv`. Depends on
**shaped-rendering** (and transitively shaped-graphics + typed-geometry + clean-core). Part of
the [graphics family](../../../docs/graphics.md) (`sv → sr → sg → tg/cc`).

sv will grow into Shaped Code's visualization renderer — the top of the graphics stack, serving
SOLIDEAN, internal tools, and customer visualization needs.

This library is an **early-stage skeleton**: it builds and links but has no renderer yet. See
[docs/structure.md](docs/structure.md) for the intended scope.

## Building & testing

Build and test through the repo driver — never run the `shaped-viewer-test` binary directly:

```bash
uv run dev.py test "sv "     # just the shaped-viewer tests while iterating
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance (empty for now).
- [docs/_index.md](docs/_index.md) — shaped-viewer's documentation hub.
- [docs/structure.md](docs/structure.md) — the intended module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — sv-specific conventions (thin for
  now), on top of the repo-wide ones.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
