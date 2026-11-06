---
created: 2026-11-05
priority: normal
tags: [xr, ar]
parent: epic-xr
---

# Passthrough: AR as an alpha-blend session

## Wanted

- Premultiplied alpha from whatever tonemaps an AR frame, clearing to transparent.
  sr has no tonemapping routine yet, so this waits for one, and `xr-cube` writes its colors directly until then.

`sxr::blend_mode` landed on 2026-11-06, with `session::blend` and `system::offers_blend`.
A runtime's own alpha blend and additive modes are tested only as a choice, so `xrEndFrame` has never been given one.
Monado's simulated headset lists opaque alone, and which modes a Quest lists has not been read.

## Known

- Over Link, passthrough reaches PCVR only in Meta's developer mode.

## What the Link prototype showed

`xr-cube` runs over Quest Link with passthrough since 2026-11-05: the runtime's `XR_FB_passthrough` layer under the projection layer.
The projection layer blends over it by `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`, from eyes cleared to transparent.
No foreign texture is involved: the runtime composites the cameras itself.
Over Link the extension is listed only once the Link app's Settings > Beta has Developer Runtime Features and Passthrough over Meta Quest Link on, and Quest Link restarted after.
