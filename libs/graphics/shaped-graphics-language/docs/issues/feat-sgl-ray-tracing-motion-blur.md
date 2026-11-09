---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/raytracing-futures.md
  - file: libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md
---

# Ray-traced motion blur: hardware where a device has it, and software by request on dx12 and vulkan

The aim is timed transforms and timed vertices at the hardware level once SGL reaches it, with a software walk on everything else.
`ray.time` is the whole of the shader's side, so a shader does not change when a target gains hardware.
Which path ran is a backend's to say, as `ctx.implementation_of` says `emulated` for webgpu's ray queries.

Done, on webgpu and the interpreter:

* **Timed transforms.** `tlas_instance::transform_at_end` (EVAL-130, EVAL-131).
* **Timed vertices.** `blas_triangles::later_poses`, one pose at time 1 for now, the topology fixed (EVAL-132).
  Its shape is `VkAccelerationStructureGeometryMotionTrianglesDataNV`'s, a second buffer of the same layout.
  A list of poses leaves room for Metal's motion, which takes several.
* **The boxes.** A BLAS box holds both poses and a TLAS box both transforms of that, so they hold every pose at every time.

Done, on dx12 and vulkan, by request:

* **An inline trace of a package that takes `SOFTWARE_TRACE`**, on every device, so a device's hardware never decides how fast a build runs (EMIT-135).
  It is a build's choice three times over: the package, `accel_build_flag::software_trace`, and the `cpu_*` spans a build reads.
  A package that did not opt in is refused a TLAS that moves when its group is made, naming the option.
* **The pool** is host-visible and written at record time, which needs no kernel, and sg compiles none on these backends.

Left:

* **Reading a build's input off the GPU**, so a caller need not keep its geometry on the CPU.
  It would need the walk's build kernels as bytecode per backend, or a readback ahead of the build.
* **A staging group or a binding array traced in software**: a snapshot carries descriptors, and the roots are none.
* **A ray-tracing pipeline's `trace_ray`** has no emulated form on dx12 and vulkan, and sees no motion.
  An inline trace inside a ray-tracing stage of an opted-in package does.
* **Tracing beside native:** a hybrid that takes the native trace for what stands still and the walk for what moves would pay software speed less often.

* **The hardware paths, none of which ran here.** Neither GPU of the development machine, an Intel Arc B580 and an AMD iGPU, has any of them.
  * **vulkan:** `VK_NV_ray_tracing_motion_blur` (NVIDIA only) takes both timed transforms and timed vertices, and reaches a pipeline's `traceRayMotionNV`, never a ray query.
    DXC has no HLSL for it, so a text body with `vk::ext_instruction` is one route.
    SPIR-V of SGL's own is the other ([epic-sgl-optimizing-passes-and-ir-targets](epic-sgl-optimizing-passes-and-ir-targets.md)).
  * ~~metal~~: landed 2026-11-09 on an M4.
    A pipeline's trace takes its ray's time natively over motion acceleration structures, keyframed vertices and instance transforms.
    Metal's query has no motion, so an inline trace of an opted-in package takes the software walk, its pool and roots at argument-table slots 28 and 29.
    The motion tags are on a package's intersectors only where it takes `SOFTWARE_TRACE`, since they double a static scene's traversal.
    Metal refuses a structure that moves to any other package, as dx12 and vulkan do.
  * **dx12:** DXR has no motion at all, so it stays with the software walk until it does.
* **SRT keyframes**, which interpolate a rotation, rather than two matrices mixed element by element.
  A large rotation shrinks an instance partway through under the mix.
* **More than two keyframes**, for instances and for vertices.
* **Moving boxes.** A procedural BLAS stands still, though an intersection can read its ray's `time` itself.
