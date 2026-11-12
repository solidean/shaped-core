---
created: 2026-11-05
priority: next
tags: [xr, design]
parent: epic-xr
closed: 2026-11-06
resolution: fixed
---

# The session API for environment depth, scene, anchors and hit tests

All four are in the first design (decided in `epic-xr`), because the intended demo app uses them.
Each is a Meta OpenXR extension with a WebXR counterpart that only partly matches, so the shape has to be settled before the session API is.

## To settle

- Depth: `XR_META_environment_depth` vs WebXR depth sensing — a texture per view, its format, and how a render pass reads it.
- Scene: Meta's spatial entities vs WebXR plane and mesh detection — what a plane, a mesh and a label are.
- Anchors: `XR_FB_spatial_entity` persistence vs WebXR anchors, which do not persist the same way.
- Hit tests: native raycasts against the scene vs WebXR's hit-test source.
- How a session reports which of these it has, the way sg reports features.
