# shaped-viewer structure (sv::)

The living roadmap for shaped-viewer. Section headers carry a status tag: **[done]** /
**[in progress]** / **[planned]**. This document is design intent, not a guarantee of final API.

shaped-viewer is currently an **empty-but-buildable skeleton**: an anchor translation unit and a
`*-test` binary, no renderer yet.

## Goals

A professional, RTX-enabled visualization renderer with a dev-friendly API, built on
shaped-rendering (`sr::`). It is the top of the graphics stack and the intended home for
Shaped Code's visualization needs (SOLIDEAN, internal tools, customer projects).

## Intended scope (all [planned])

```text
scene / renderer API   [planned]  the dev-friendly entry point
RTX / ray tracing      [planned]  hardware-accelerated path
materials / lighting   [planned]
camera / controls      [planned]
viewport / windowing   [planned]
```

The exact module layout settles as the renderer takes shape; keep this roadmap updated as it does.
