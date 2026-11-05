---
created: 2026-11-05
priority: normal
tags: [xr, ar]
parent: epic-xr
blocked-by: [feat-shaped-xr-core]
---

# Passthrough: AR as an alpha-blend session

## Wanted

- `xr::blend_mode`: `opaque`, `alpha_blend`, `additive`.
- On Quest, alpha blend rides `XR_FB_passthrough` beneath the projection layer.
- sr's tonemapping writes premultiplied alpha into an sRGB target, clearing to transparent.

## Known

- Over Link, passthrough reaches PCVR only in Meta's developer mode.

## What the Link prototype showed

`xr-cube` runs over Quest Link with passthrough since 2026-11-05: the runtime's `XR_FB_passthrough` layer under the projection layer.
The projection layer blends over it by `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`, from eyes cleared to transparent.
No foreign texture is involved: the runtime composites the cameras itself.
Over Link the extension is listed only once the Link app's Settings > Beta has Developer Runtime Features and Passthrough over Meta Quest Link on, and Quest Link restarted after.
