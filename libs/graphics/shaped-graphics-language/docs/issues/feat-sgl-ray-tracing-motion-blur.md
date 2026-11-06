---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/raytracing-futures.md
  - file: libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md
---

# Ray-traced motion blur: hardware where a device has it, and software on the dx12 and vulkan devices without

The aim is timed transforms and timed vertices at the hardware level once SGL reaches it, with a software walk on everything else.
`ray.time` is the whole of the shader's side, so a shader does not change when a target gains hardware.
Which path ran is a backend's to say, as `ctx.implementation_of` says `emulated` for webgpu's ray queries.

Done, on webgpu and the interpreter:

* **Timed transforms.** `tlas_instance::transform_at_end` (EVAL-130, EVAL-131).
* **Timed vertices.** `blas_triangles::later_poses`, one pose at time 1 for now, the topology fixed (EVAL-132).
  Its shape is `VkAccelerationStructureGeometryMotionTrianglesDataNV`'s, a second buffer of the same layout.
  A list of poses leaves room for Metal's motion, which takes several.
* **The boxes.** A BLAS box holds both poses and a TLAS box both transforms of that, so they hold every pose at every time.

Left:

* **The software walk on dx12 and vulkan.** Neither answers `ray_tracing_motion_blur`, so a moving instance or geometry there is refused at the build.
  An inline trace could take the walk the prelude already has, and three things stand in the way:
  * **What picks the walk.** The legalizer keeps a trace's form by target (EMIT-135), so dx12 and vulkan always write the native one.
    Choosing the emulated form needs an input of the compile: a shader `option`, a feature, or a mode of the package.
    Whichever it is decides who pays when one device has motion and another does not, so it is a language and host question, not an emitter's.
  * **The pool.** sg's pool is webgpu's, and its build kernels are WGSL text a WebGPU device compiles.
    The dx12 and vulkan backends compile no shader text: sg does not depend on a compiler (slib's edges do).
    So they need those kernels as committed bytecode per backend, kernels in slib or shaped-rendering, or a CPU build from data the caller also hands over.
  * **The binding.** HLSL needs EMIT-135's pool and roots in sg's reserved group, which dx12's root signature and vulkan's set layout then carry.
  A ray-tracing pipeline's trace has no emulated form and stays refused.
* **The hardware paths, none of which ran here.** Neither GPU of the development machine, an Intel Arc B580 and an AMD iGPU, has any of them.
  * **vulkan:** `VK_NV_ray_tracing_motion_blur` (NVIDIA only) takes both timed transforms and timed vertices, and reaches a pipeline's `traceRayMotionNV`, never a ray query.
    DXC has no HLSL for it, so a text body with `vk::ext_instruction` is one route.
    SPIR-V of SGL's own is the other ([epic-sgl-optimizing-passes-and-ir-targets](epic-sgl-optimizing-passes-and-ir-targets.md)).
  * **metal:** the intersector takes a time over motion acceleration structures, with vertex keyframes, which is Mac work.
  * **dx12:** DXR has no motion at all, so it stays with the software walk until it does.
* **SRT keyframes**, which interpolate a rotation, rather than two matrices mixed element by element.
  A large rotation shrinks an instance partway through under the mix.
* **More than two keyframes**, for instances and for vertices.
* **Moving boxes.** A procedural BLAS stands still, though an intersection can read its ray's `time` itself.
