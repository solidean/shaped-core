---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# The native layer lowers ray queries and ray-tracing stages

Ray queries first, then the pipeline stages, then what the text route cannot reach: motion traces through VK_NV_ray_tracing_motion_blur and SER written directly.

## Measured on ray queries

On 2026-11-07, on the Arc B580 and over the repo's SGL, DXC 1.9.2602:

- **Every vertex and pixel entry point lowers on both writers**, `trace_pixel_ps` among them: 82 of 82.
  Compute went from 93 to 105 of 115 on DXIL, and from 98 to 110 on SPIR-V.
  Each passes DXC's validator and `spirv-val`.
- **sg's ray query tests pass on `dx12-native-dxil` and `vulkan-native-spirv`**, with vulkan's validation and sync validation on.
- **The mixed test holds a read of every value a query answers to DXC's, within 1e-4**, on dx12 and vulkan.
  It is [`mixed-route-query-test.cc`](../../../shaped-graphics/tests/raster/mixed-route-query-test.cc).
  A compute stage and a query in a loop compare stage against stage, and a pixel stage in each pairing with its vertex stage.
  It can fail: taking the next row of a transform in lowering logged 60 failed checks.
- **Two shapes DXC's optimizer hid from the validator came up**, and both are general.
  A `select` must not read `undef`, so `flatten_branches` takes the other way's value.
  An operation declared and left with no call is refused, so `remove_unused` drops its declaration.

Not done here: a query with opacity micromaps, and `@first_instance`-style reads nothing asks for, such as a query's own flags or t_min.
No GPU but the B580 ran this, and the Agility runtime did not.

## What the pipeline stages need, measured

Not started on either writer.
A library is a different module and a different container, so this is several times the size of queries, and the DXIL half is most of it.
These are `lib_6_9` compiles of [`raytracing_end_search.sgl`](../../../shaped-graphics/tests/shaders/sgl/raytracing_end_search.sgl) on DXC 1.9.2602, read from their disassembly and their parts.

**The container.** `SFI0`, `VERS`, `RDAT`, `STAT`, `HASH` and `DXIL`, with no `PSV0` and no signature.
`RDAT` is new: 156 bytes for one export with no resource, and 268 for one with two resources.
Whether the validator compares it to the module, as it does `PSV0`, and whether `STAT` and `VERS` may go, are not tried.

**The module.** `dx.shaderModel` is `lib`, and `dx.entryPoints` opens with a null entry.

- **A resource is a global.** `external constant %dx.types.Handle`, loaded at the top of the function, then `createHandleForLib` (160) and `annotateHandle`.
  `dx.resources` names that global through a constant `bitcast`, which `module_builder` cannot express yet.
- **An entry point takes pointers.** `void (%struct.layers*, %struct.slab_attributes*)`, `noalias nocapture`, to named structs.
  A function's parameters and named struct types are both new to the builder.
- **Its name is mangled**, `\01?stop_at_slab@@YAX...`, and `RDAT` states the plain name, which is what sg's state object exports.
- **Three more metadata lists.** `dx.typeAnnotations` states each struct's layout and each function's parameters.
  The entry's properties state the kind, the payload size and the attribute size: `{8, 10, 6, 8, 7, 4, 5, {0}}` is a closest hit of an 8-byte payload and 4 bytes of attributes.
  `dx.dxrPayloadAnnotations` carries `[raypayload]`'s access qualifiers, which the text route writes (EMIT-137).
- **A payload lives in an `alloca` of the caller**, which `dx.op.traceRay.struct.layers` (157) takes by pointer, so the operation overloads on a named struct.

**The design this suggests.**

- A stage past `raygen` reaches `lowering` as a stage of no elements.
  The flat tree already reads the launch and the hit through `launch_index`, `hit_*` and the rest, so those become one `op::ray_read` with a `sub`, as `op::ray_query_read` is.
- A payload, a hit's attributes and a callable's data are one global each, of a new `storage`.
  The writers differ only in what that global is: a parameter on DXIL, and a variable of `IncomingRayPayloadKHR`, `HitAttributeKHR` or `IncomingCallableDataKHR` on SPIR-V.
  `op::trace_ray` and `op::call_callable` take a local, which SPIR-V copies through a `RayPayloadKHR` global, as EMIT-225's text does.
- `IgnoreHit`, `AcceptHitAndEndSearch` and `ReportHit` are the new terminators and effects: `OpIgnoreIntersectionKHR` and `OpTerminateRayKHR` end a block on SPIR-V.
- A hit object stays out of it at first, as a named gap on both writers, so CHK-516 and EMIT-225 hold by no hit object lowering at all.
- `dxil/impl/library.cc` should hold what a library adds, and `container.cc` a second part list, so `compile_dxil` stays one function.

**The order.** SPIR-V's stages first as a commit, since they need no new container, and DXIL's behind them, both before the slice counts as done.
The mixed test should try a native ray generation stage against DXC's hit groups, which a state object may or may not take, and that answer is open on both APIs.
