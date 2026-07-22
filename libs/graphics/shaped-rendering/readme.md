# shaped-rendering

Concrete render routines and helpers on top of [shaped-graphics](../shaped-graphics/readme.md).
Namespace `sr`. Depends on **shaped-graphics** and **shaped-shader-library** (concrete routines acquire
their shaders through it) and the vendored **imgui**, plus transitively typed-geometry + clean-core.
Part of the [graphics family](../../../docs/graphics.md) (`sv → sr → sg → tg/cc`).

sr is the home for the common building blocks of a renderer built on sg — mipmap generation, texture compression, tonemapping, and similar reusable render routines.
It is also home to the **window abstraction**, where the graphics family meets the OS.

## Windows

`sr::window_system` owns the platform init and the event pump; `sr::window` is one OS window.
A window's `native_window_handle()` is exactly what `sg::swapchain_description` wants, so a windowed renderer can be written against sr alone:

```cpp
auto const wsys = sr::window_system::create();
auto const win = wsys->create_window({.title = "viewer", .width = 1600, .height = 900});
auto const sc = ctx->create_swapchain({.native_window_handle = win->native_window_handle()});

while (!win->is_close_requested())
{
    wsys->poll_events();
    if (win->is_minimized())
        continue;
    // ... acquire, render, submit_command_list_and_present ...
}
```

Three things worth knowing before you use it:

* **SDL3 backs it, and SDL3 is fetched on demand — so the backend is optional, not the API.**
  Without one, `window_system::try_create` fails with a reason instead of the types disappearing, so the same
  code compiles either way. `SR_HAS_WINDOW` (1/0) answers "is a backend compiled in" when you need it at
  compile time. No SDL type reaches a public header — see [coding-guidelines](docs/coding-guidelines.md).
* **`window_system` is main-thread bound.**
  Create it, create windows from it, poll it and destroy it all on one thread.
  On macOS that thread must be the process main thread.
  Violations assert.
* **Rendering is not.**
  `native_window_handle()` is fixed for a window's lifetime, so a render thread may drive the swapchain as long as `poll_events` stays on the main thread.

Multiple windows work today: each has its own size, position, focus, close latch and native handle, and `wsys->windows()` enumerates them.
That is what imgui's multi-viewport support is built on — see [docs/imgui.md](docs/imgui.md).

## Input

`poll_events()` also collects what the user did, drained as one globally-ordered stream:

```cpp
for (auto const& e : wsys->events())
    if (auto const* k = std::get_if<sr::key_event>(&e.payload))
        if (k->is_down && k->scancode == sr::scancode::escape)
            e.window->request_close();
```

Keyboard events carry both a **physical** `scancode` (position, so WASD survives AZERTY) and the layout-mapped
`character`, because movement wants position while a ctrl+Z-style shortcut wants the letter.
Text is separate: `text_event` delivers committed UTF-8 after `window::start_text_input()`, which is the only
thing that handles IME composition, dead keys and paste correctly — never rebuild text from key events.
`window::set_relative_mouse_mode()` captures the cursor for an FPS-style camera.

See the [cheat-sheet](cheat-sheet.md) for the full surface.

It also hosts the **Dear ImGui renderer** — the renderer half of an imgui backend, drawn entirely
through sg with no native graphics calls. See [docs/imgui.md](docs/imgui.md).

The render-routine **framework** (the `sg::render_routine` base with 3-phase, hot-reload-aware init,
and the per-context `ctx.routines` registry) lives in **shaped-graphics** — see its
[docs/render-routines.md](../shaped-graphics/docs/render-routines.md). `sr` hosts the **concrete**
routines built on top of it; they land as they are implemented. See
[docs/render-routines.md](docs/render-routines.md) for the sr-side overview and
[docs/structure.md](docs/structure.md) for the wider roadmap.

## Building & testing

Build and test through the repo driver — never run the `shaped-rendering-test` binary directly.
The render-routine framework is tested in shaped-graphics, where it lives; concrete routines bring their own tests as they land.

```bash
uv run dev.py test "sr"                 # everything in shaped-rendering
uv run dev.py test "sr - "              # the window suite — headless, runs anywhere
uv run dev.py test "sr::impl"           # the imgui arithmetic — no device needed, runs everywhere
uv run dev.py test "sr::imgui_routine"  # imgui end to end on a dx12 WARP device (skips without one)
uv run dev.py test "sg - routine"       # the render-routine framework tests (in shaped-graphics)
```

The window suite runs on SDL's dummy video driver, so it needs no display.
What that cannot reach — a real window manager delivering close and resize events, and a real native handle — lives in the manual bucket and needs a display:

```bash
uv run dev.py test "sr - window native handle (manual)" --manual   # creates a hidden real window, checks the handle
uv run dev.py test "sr - window (manual)" --manual --mirror-test-output --timeout 0   # opens a window; close it to end
```

`--timeout 0` matters for any test that waits on a person: dev.py kills a test binary after 60s by default and reports it failed.
The same applies to the imgui one, `uv run dev.py test "sr - imgui window (manual)" --manual --mirror-test-output --timeout 0`.

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [docs/render-routines.md](docs/render-routines.md) — sr-side render-routine overview (+ link to the framework).
- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — shaped-rendering's documentation hub.
- [docs/structure.md](docs/structure.md) — the intended module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — sr-specific conventions (thin for
  now), on top of the repo-wide ones.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
