---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_raytracing.cc
---

# sg webgpu: every acceleration-structure build dispatch creates a bind group

The ray-query polyfill builds a BLAS or TLAS as a run of compute dispatches, and `dispatch_acceleration_kernel` creates a fresh `WGPUBindGroup` for each one.
Firefox frees a bind group only when it collects garbage.
That is the churn [bug-sg-webgpu-per-frame-objects-exhaust-firefox](bug-sg-webgpu-per-frame-objects-exhaust-firefox.md) fixed for the transient path.

Measured, in Firefox on the web-viewer with the modern hall: a TLAS rebuilt every frame cost about 36 bind groups a frame, never freed between collections.
The collections showed as 85 ms stalls, and the frame rate fell from about 100 to 65 fps.

sv's bound raster mode now keeps its TLAS while the instances are unchanged, so a still scene no longer rebuilds.
An animated scene still would, and so would any other caller rebuilding per frame.

Wanted: the build's groups reused, as the transient bind groups are.
Only the constant-page placement varies between dispatches, so one group per kernel with a dynamic offset for the arguments would likely do.
