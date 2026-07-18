# shaped-rendering structure (sr::)

The living roadmap for shaped-rendering. Section headers carry a status tag: **[done]** /
**[in progress]** / **[planned]**. This document is design intent, not a guarantee of final API.

## Goals

Reusable render routines and helpers built on shaped-graphics (`sg::`) — the common code a
renderer needs, factored out of any single renderer so both internal tools and shaped-viewer
can share it.

## Render-routine framework **[done]**

The composable base every routine builds on — see [render-routines.md](render-routines.md):

```text
render_routine          [done]  one unit of GPU work; 3-phase init (once/declare/materialize), re-inits on reload
routine_handle<R>       [done]  cheap handle; acquire(ctx, cmd) is the static-execute entry point
render_routine_package  [done]  hand-written group of routines; members + a package dependency system (dedup, cycle-checked)
render_routine_library  [done]  the one object you keep around: owns packages, fan-out init, watches slibs for hot reload
```

## Intended scope (routines, all [planned])

```text
mipmap generation      [planned]
texture compression    [planned]
tonemapping            [planned]
render passes / helpers [planned]
common shader utilities [planned]
```

The exact module layout settles as the first routines land; keep this roadmap updated as it does.
