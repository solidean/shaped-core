---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/raytracing-futures.md
---

# Ray-traced motion blur: a native trace where a device has it, moving BLASes, and keyframes beyond two

A ray carries a `time`, and a TLAS instance may move from one transform at time 0 to another at time 1 (EVAL-130, EVAL-131).
The emulated trace places it at the ray's time, so webgpu and a test on the interpreter see motion; sg answers `ray_tracing_motion_blur` on webgpu alone.

Left:

* **A native trace.** DXR has no motion at all.
  vulkan's `VK_NV_ray_tracing_motion_blur` (NVIDIA only) reaches a pipeline's `traceRayMotionNV`, never a ray query.
  DXC has no HLSL for it, so a text body with `vk::ext_instruction` is the route there.
  Metal's intersector takes a time over motion acceleration structures, which is Mac work.
  Neither GPU of the development machine, an Intel Arc B580 and an AMD iGPU, has the NV extension, so no native motion has run.
* **dx12 and vulkan through the emulated walk.** They build no pool, so a moving instance there is refused rather than traced in software.
  Building the pool beside the native structure would lift that, at its cost.
* **A BLAS whose vertices move**, and SRT keyframes, which interpolate rotation, rather than two matrices mixed element by element.
* **More than two keyframes.**
* **The webgpu GPU test** of the moving pool layout runs only in a browser (`dev.py test-web`), which this change compiled but did not run.
