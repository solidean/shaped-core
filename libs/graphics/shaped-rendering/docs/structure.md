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
fullscreen / borderless [planned]
HiDPI                  [planned]  SDL_WINDOW_HIGH_PIXEL_DENSITY once a per-monitor-DPI UI needs it
gamepad                [planned]  SDL_JOYSTICK is currently compiled out; see TODO
imgui multi-viewport   [planned]  the reason the multi-window mechanism is already in place
```

The window API is optional: SDL3 is fetched on demand, and CMake defines `SR_HAS_WINDOW` to `1` or `0`.

## Intended scope (routines, all [planned])

```text
mipmap generation      [planned]
texture compression    [planned]
tonemapping            [planned]
render passes / helpers [planned]
common shader utilities [planned]
```

The exact module layout settles as the first routines land; keep this roadmap updated as it does.
