---
created: 2026-11-13
tags: [vulkan, raster]
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_context.create.cc
---

# A pipeline blending its color targets differently is not refused on a vulkan device without independentBlend

sg's vulkan context did not enable `independentBlend`, so a raster pipeline whose color targets blend differently failed validation on every vulkan device.
The differential corpus's `blended` setup found it on 2026-11-13; the context now enables the feature wherever the device has it.

What is left: a device without it, rare but real on mobile, still takes such a pipeline and builds an invalid one.
dx12, metal and webgpu blend per target in core, so the refusal is vulkan's alone.
sg should refuse that pipeline there, naming the feature, as it refuses a wireframe pipeline on a device without `fillModeNonSolid`.
