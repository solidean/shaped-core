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
                                reached by type via static acquire(cmd) — no handle, no registration
ctx.routines            [done]  per-context registry: lazy self-registration, prewarm<...>() / evict<R>() / clear()
sg::reload_generation   [done]  process-global hot-reload counter (bumped by the shader library on reload)
```

`sr` itself hosts the **concrete** routines below.

## Intended scope (routines, all [planned])

```text
mipmap generation      [planned]
texture compression    [planned]
tonemapping            [planned]
render passes / helpers [planned]
common shader utilities [planned]
```

The exact module layout settles as the first routines land; keep this roadmap updated as it does.
