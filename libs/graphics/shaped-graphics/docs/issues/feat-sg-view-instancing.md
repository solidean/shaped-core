---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/capabilities.hh
---

# `sg::feature::view_instancing`: one draw rendering every view

On Quest's Adreno GPU, multiview is the only fast stereo: the driver bins both views together, where `@layer` rasterises them separately.

## Wanted

- A feature beside `vertex_layer_output`, and a `view_count` on the raster pass and pipeline.
- dx12 view instancing (`SV_ViewID`), vulkan `VK_KHR_multiview`, metal vertex amplification.
- webgpu reports it unsupported; the fallback is a draw per view.

## Known

- The shader side is `feat-sgl-view-index`.
