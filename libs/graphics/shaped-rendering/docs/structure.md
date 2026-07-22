# shaped-rendering structure (sr::)

The living roadmap for shaped-rendering. Section headers carry a status tag: **[done]** /
**[in progress]** / **[planned]**. This document is design intent, not a guarantee of final API.

## Goals

Reusable render routines and helpers built on shaped-graphics (`sg::`) — the common code a
renderer needs, factored out of any single renderer so both internal tools and shaped-viewer
can share it.

## Render-routine framework **[done, in shaped-graphics]**

The composable base every routine builds on lives in **shaped-graphics**, not here — see
[shaped-graphics/docs/render-routines.md](../../shaped-graphics/docs/render-routines.md):

```text
sg::render_routine<D>   [done]  one unit of GPU work; 3-phase init (once/declare/materialize), re-inits on reload
                                reached by type via static acquire(cmd) / prewarm(ctx) / evict(ctx) — no handle,
                                no registration
ctx.routines            [done]  per-context registry: lazy self-registration, clear()
sg::reload_generation   [done]  process-global hot-reload counter (bumped by the shader library on reload)
```

`sr` itself hosts the **concrete** routines below.

## Windowing **[done]**

The OS window abstraction, backed by SDL3 and exposing none of it.
This is where the graphics family meets the operating system.

```text
sr::window_system      [done]  platform init + the event pump; creates windows; main-thread bound, one per process
sr::window             [done]  one OS window: native handle, pixel size, minimized state, latched close request
multiple windows       [done]  per-window state and id-keyed dispatch; wsys->windows() enumerates them
sr::input_event        [done]  keyboard (physical + character), text/IME, mouse move/button/wheel, relative mode
                               one globally-ordered stream per frame, drained through wsys->events()
sr::cursor_shape       [done]  the pointer's shape, set on the system (process-global, like the platform's)
clipboard              [done]  get/set text on the system clipboard
window position       [done]  position()/set_position(); write-through, so a get right after a set reads it
window focus          [done]  is_focused() as of the last poll, focus() to ask for it
borderless / topmost  [done]  window_description: has_decoration, is_always_on_top, has_taskbar_icon, is_focusable
fullscreen            [planned]
HiDPI                  [planned]  SDL_WINDOW_HIGH_PIXEL_DENSITY once a per-monitor-DPI UI needs it
gamepad                [planned]  SDL_JOYSTICK is currently compiled out; see TODO
```

The API is always present. Only the backend is optional, since SDL3 is fetched on demand — without one,
`window_system::try_create` fails and `SR_HAS_WINDOW` is 0.

## Dear ImGui **[done]**

The renderer half of an imgui backend, drawn entirely through `sg` — no native graphics calls.
See [imgui.md](imgui.md) for the shape and the reasoning.

```text
sr::imgui_context     [done]  owns the ImGui context + the frame bracket; docking on, viewports stubbed
sr::imgui_routine     [done]  the render routine: atlas textures, geometry, pipelines; one execute()
sr::shader_package()  [done]  sr's shader package, to register with a slib::shader_library
```

Still open: `ImDrawCmd::UserCallback` dispatch and an Alpha8 atlas path.
Multi-viewport is no longer blocked now that windowing has landed — see the windowing section above.

## Intended scope (routines, all [planned])

```text
mipmap generation      [planned]
texture compression    [planned]
tonemapping            [planned]
render passes / helpers [planned]
common shader utilities [planned]
```

The exact module layout settles as more routines land; keep this roadmap updated as it does.
