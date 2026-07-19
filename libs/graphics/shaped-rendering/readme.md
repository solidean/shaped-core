# shaped-rendering

Concrete render routines and helpers on top of [shaped-graphics](../shaped-graphics/readme.md).
Namespace `sr`. Depends on **shaped-graphics** and **shaped-shader-library** (concrete routines acquire
their shaders through it), plus transitively typed-geometry + clean-core.
Part of the [graphics family](../../../docs/graphics.md) (`sv → sr → sg → tg/cc`).

sr is the home for the common building blocks of a renderer built on sg — mipmap generation,
texture compression, tonemapping, and similar reusable render routines.

The render-routine **framework** (the `sg::render_routine` base with 3-phase, hot-reload-aware init,
and the per-context `ctx.routines` registry) lives in **shaped-graphics** — see its
[docs/render-routines.md](../shaped-graphics/docs/render-routines.md). `sr` hosts the **concrete**
routines built on top of it; they land as they are implemented. See
[docs/render-routines.md](docs/render-routines.md) for the sr-side overview and
[docs/structure.md](docs/structure.md) for the wider roadmap.

## Building & testing

Build and test through the repo driver. `sr` has no test binary yet (the framework is tested in
shaped-graphics, where it lives; concrete routines bring their own tests as they land):

```bash
uv run dev.py build -t shaped-rendering      # build the library while iterating
uv run dev.py test "sg - routine"            # the render-routine framework tests (in shaped-graphics)
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [docs/render-routines.md](docs/render-routines.md) — sr-side render-routine overview (+ link to the framework).
- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — shaped-rendering's documentation hub.
- [docs/structure.md](docs/structure.md) — the intended module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — sr-specific conventions (thin for
  now), on top of the repo-wide ones.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
