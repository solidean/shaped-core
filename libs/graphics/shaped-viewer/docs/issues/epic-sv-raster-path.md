---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/docs/raster.md
  - symbol: sv::raster_routine
---

# The raster path past its first slice

[raster.md](../raster.md) is the design; what is left of the first slice:

- **Quadric batches are not drawn**, and the layer warns once: [feat-sv-raster-quadric-batches](feat-sv-raster-quadric-batches.md).
- **No antialiasing**: every pixel shades at its center, so edges alias where the tracer's jitter antialiases them.
- **Coverage is cut at one half**, with no transparency, and transmission refracts nothing.
  Decided: depth peeling for glass and alpha clipping for cutouts ([the roadmap](../plans/rendering-roadmap.md#decided-for-the-raster-path-and-the-hybrid)).
- **A rect light visible to the camera is not drawn.**
- **No depth prepass**, so an occluded surface is shaded before the one in front of it lands; and no vertex reuse, since no index buffer is bound.
- **A vulkan rendering scope splits on the first draw after a fresh upload**: sg has no way to record a draw's accesses before its scope opens.
- **Only dx12 can pretend a device has no ray tracing** (`dx12_config::withhold_raytracing`), so the no-ray-tracing test runs there alone.
  Vulkan's arm is untested on such a device; the same knob on `vulkan_config` would reach it.
