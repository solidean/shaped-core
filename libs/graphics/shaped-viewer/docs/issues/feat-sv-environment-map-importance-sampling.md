---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/background.hh
  - file: libs/graphics/shaped-viewer/docs/lights.md
---

# The environment cannot produce a sharp reflection

The background is an order-3 SH probe, so a smooth specular lobe reflects a blur whatever the roughness says.
Only the analytic lights — rects and suns — give a real highlight.
An equirect HDR environment with 2D-CDF importance sampling is the fix; babel already reads it, through `babel::hdr` and `babel::image`.
How such a map relates to a sun light is open — see "Designed, not landed" in [lights.md](../lights.md).
