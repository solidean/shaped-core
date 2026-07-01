# shaped-rendering structure (sr::)

The living roadmap for shaped-rendering. Section headers carry a status tag: **[done]** /
**[in progress]** / **[planned]**. This document is design intent, not a guarantee of final API.

shaped-rendering is currently an **empty-but-buildable skeleton**: an anchor translation unit and
a `*-test` binary, no routines yet.

## Goals

Reusable render routines and helpers built on shaped-graphics (`sg::`) — the common code a
renderer needs, factored out of any single renderer so both internal tools and shaped-viewer
can share it.

## Intended scope (all [planned])

```text
mipmap generation      [planned]
texture compression    [planned]
tonemapping            [planned]
render passes / helpers [planned]
common shader utilities [planned]
```

The exact module layout settles as the first routines land; keep this roadmap updated as it does.
