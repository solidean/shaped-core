# shaped-viewer

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API, built on [shaped-rendering](../shaped-rendering/readme.md).
Namespace `sv`.
Depends on **shaped-rendering** (and transitively shaped-graphics + typed-geometry + clean-core).
Part of the [graphics family](../../../docs/graphics.md) (`sv → sr → sg → tg/cc`).

sv will grow into Shaped Code's visualization renderer — the top of the graphics stack, serving SOLIDEAN, internal tools, and customer visualization needs.

A **view is the definition of one texture**, and one of a view's layers may be a whole layout tree — so views nest, at any depth.
A frame is authored through fluent handles, flattened into a `render_plan`, and replayed by `viewer_renderer`.
Raytracing-first, dx12 + DXR today.

```cpp
for (auto f : sv::interactive("my viewer"))
{
    auto rows = f.window().view().layout_rows({.spacing = 6});
    rows.add_view("left").add_scene().add_mesh(mesh);   // content-addressed: an unchanged mesh uploads nothing
    rows.add_view("right").add_scene().add_mesh(mesh);
}
```

No context is threaded through — one is acquired through the provider `sv::set_acquire_context` installs, or from a built-in default.

An application whose own loop must stay in charge writes the same loop out, with `begin_frame` / `end_frame` around the identical authoring calls.
Both author the same `sv::frame`; only what ends it differs — the range yields an `sv::frame_scope` that presents when the loop body ends, `begin_frame` hands out the frame itself:

```cpp
auto viewer = sv::viewer::create("my viewer");
while (viewer.is_running())
{
    auto& f = viewer.begin_frame();
    if (!f) // the window cannot draw right now (minimized)
        continue;
    f.add_scene().add_mesh(mesh);
    viewer.end_frame();
}
```

See [docs/structure.md](docs/structure.md) for the roadmap and [cheat-sheet.md](cheat-sheet.md) for the API.

## Building & testing

Build and test through the repo driver — never run the `shaped-viewer-test` binary directly:

```bash
uv run dev.py test "sv "     # just the shaped-viewer tests while iterating
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — shaped-viewer's documentation hub.
- [docs/structure.md](docs/structure.md) — the intended module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — sv-specific conventions (thin for now), on top of the repo-wide ones.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
