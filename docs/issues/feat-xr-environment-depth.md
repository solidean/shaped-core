---
created: 2026-11-06
priority: normal
tags: [xr, ar]
parent: epic-xr
blocked-by: [feat-xr-capabilities]
---

# Environment depth: frame data and an SGL module

Done when your hand occludes `xr-cube` in the headset.
Design: [AR capabilities](../../libs/graphics/shaped-xr/docs/design.md#ar-capabilities).

## Wanted

- `XR_META_environment_depth` behind `sxr::capability::environment_depth`.
- `frame.environment_depth`, none until the first depth arrives: a texture array, a world-to-depth-clip matrix per view, and the near and far distances.
  The matrices come from the depth map's own pose and fov, since Meta requires reprojecting into it.
- An exported SGL module, `sxr_depth`, the way sr exports `slug`, so `sxr_depth.visibility(world_position, view)` is one line of an app's material.
  It owns the encoding, the reprojection and the soft edge.
- Hand removal as a session setting, where the system supports it.

## Unknown

- The texel format, resolution and update rate: Meta's page states none of them, and the runtime creates the swapchain, so the backend must learn the format to wrap it.
- The resting layout of a depth image on vulkan, and on dx12 whether it needs a copy as the color images do.
  Meta's native samples should settle both.
- Whether the data arrives over Link: `question-xr-link-ar-probe`.
