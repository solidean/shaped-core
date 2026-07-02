# shaped-rendering

Render routines and helpers on top of [shaped-graphics](../shaped-graphics/readme.md).
Namespace `sr`. Depends on **shaped-graphics** (and transitively typed-geometry + clean-core).
Part of the [graphics family](../../../docs/graphics.md) (`sv → sr → sg → tg/cc`).

sr is the home for the common building blocks of a renderer built on sg — mipmap generation,
texture compression, tonemapping, and similar reusable render routines.

This library is an **early-stage skeleton**: it builds and links but has no routines yet. See
[docs/structure.md](docs/structure.md) for the intended scope.

## Building & testing

Build and test through the repo driver — never run the `shaped-rendering-test` binary directly:

```bash
uv run dev.py test "sr "     # just the shaped-rendering tests while iterating
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance (empty for now).
- [docs/_index.md](docs/_index.md) — shaped-rendering's documentation hub.
- [docs/structure.md](docs/structure.md) — the intended module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — sr-specific conventions (thin for
  now), on top of the repo-wide ones.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
