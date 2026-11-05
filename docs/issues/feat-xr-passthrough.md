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
