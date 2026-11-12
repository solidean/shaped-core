---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/capabilities.hh
closed: 2026-11-05
resolution: fixed
---

# `sg::feature::view_instancing`: one draw rendering every view

On Quest's Adreno GPU, multiview is the only fast stereo: the driver bins both views together, where `@layer` rasterises them separately.

## Wanted

- A feature beside `vertex_layer_output`, and a `view_count` on the raster pass and pipeline.
- dx12 view instancing (`SV_ViewID`), vulkan `VK_KHR_multiview`, metal vertex amplification.
- webgpu reports it unsupported; the fallback is a draw per view.

## Known

- The shader side is `feat-sgl-view-index`.

## Done (2026-11-05)

- `sg::feature::view_instancing`; `raster_pipeline_description::view_count` and `rendering_info::view_count`, checked against each other at bind.
- dx12: the pipeline is created through a pipeline-state stream with a view-instancing subobject, and a rendering sets the view-instance mask.
- vulkan: `multiview` enabled wherever the device has it, and the view mask on the pipeline and the rendering.
- Pixel-tested on an Intel Arc B580 (dx12 and vulkan) and an AMD iGPU (vulkan): one draw of two views, each into its own layer.
- metal and webgpu are `feat-sg-view-instancing-metal-webgpu`.
