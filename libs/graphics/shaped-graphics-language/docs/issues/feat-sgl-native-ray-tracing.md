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

Not done here: reads nothing asks for, such as a query's own flags or t_min.
No GPU but the B580 ran this, and only on the system runtime.

## What a pipeline stage is, measured

These are compiles of the repo's SGL on DXC 1.9.2602 and runs on the Arc B580, on 2026-11-07.

**A stage's memory is one global per use**, of a `storage` of its own, and the writers differ only in what that global is.

| global | SPIR-V | DXIL |
|---|---|---|
| `ray_input`, the payload or a callable's parameter | `IncomingRayPayloadKHR`, `IncomingCallableDataKHR` | the function's first parameter |
| `hit_attribute` | `HitAttributeKHR` | a hit stage's second parameter, an intersection's `alloca` |
| `ray_output`, `callable_output`, one per type | `RayPayloadKHR`, `CallableDataKHR` with a `Location` | an `alloca` |

A trace stores into its global, traces, and loads back, which is what EMIT-225's text does on vulkan.
What a stage reads of its launch, its ray and its hit is a `stage_element` of a new `built_in`, which is SPIR-V's `BuiltIn` and a `dx.op` on DXIL.
A hit's transform is the one element that is a matrix.

**A DXIL library needs less than DXC writes.**

- **The container is `SFI0`, `RDAT` and `DXIL`.** `STAT`, `VERS` and `HASH` can all go: the validator passes it, and `ID3D12StateObject` creation takes it under the bypass digest.
  That holds on the system runtime and on Agility 1.620.1.
- **The validator does compare `RDAT` with the module**, as it does `PSV0`, and refuses a mismatch: `Container part 'Runtime Data (RDAT)' does not match expected for module`.
  So every field is what the module implies.
  A hit's geometry index is raytracing tier 1.1 and shader model 6.5 there, which the flags and the row's target must say, as a query's do.
  An intersection states no attribute size, though it reports attributes.
- **`createHandleFromBinding` does not work in a library.** The validator took the operation, and no `RDAT` we could write matched it, since a function's resource list comes from what uses a global.
  So a resource is an `external constant %dx.types.Handle`, loaded and handed to `createHandleForLib`, and `dx.resources` names it through a constant `bitcast`.
- **`noreturn` is required.** `unreachable` after `ignoreHit` is refused unless the operation carries it.
- **What it can leave out:** mangled names, `dx.typeAnnotations`, and `noalias` and `nocapture`.
  `dx.dxrPayloadAnnotations` was on this list until NVIDIA's driver proved it otherwise (EMIT-137).
  A plain export name stands in `RDAT` as both names, and the validator and state object creation both take it.
- **A nonuniform index is `!dx.nonuniform` on the `getelementptr`**, an attachment the builder writes in a block of its own.

**Pipelines mix freely on both APIs.**
Each of the 15 ways of taking raygen, miss, hit and callable stages from SGL's binary, the rest from DXC's, creates one pipeline and traces alike, on dx12 and vulkan, every lane within 1e-4.
It is [`mixed-route-ray-test.cc`](../../../shaped-graphics/tests/pipeline/mixed-route-ray-test.cc).
Its fixture is [`ray_stages.sgl`](../../../shaped-graphics/tests/shaders/sgl_mixed/ray_stages.sgl): a trace from a closest hit, all six stages, a callable picked per launch.
DXC's stages state `[raypayload]` and ours do not, and neither runtime minded.
It can fail: reading the instance's index for its id in lowering logged 60 failed checks.

**The sweep.**

- **SPIR-V lowers 60 of the 65 ray-tracing entry points, and DXIL 52.** Each passes `spirv-val` or DXC's validator.
- **The five SPIR-V leaves are metal's**, an intersection fused with its any hit (EMIT-139), which has no stage on these targets.
- **DXIL leaves those and eight `sgl_empty_closest_hit`**, a closest hit of no payload that slib builds for metal alone.
- **`shaped-graphics-test`'s ray-tracing tests pass on `dx12-native-dxil` and `vulkan-native-spirv`**, with the debug layer and validation on.

**One thing the work found.** `k_storage_classes`, the stride of `lowering::pointer_type`'s table, was a written 7.
So a new `storage` made two pointer types collide, and a local took another's.
It is the enum's size now.

## Real shaders, measured

On 2026-11-07, on the Arc B580, with every native gap turned into an error for the runs below, so nothing fell back.

- **The viewer's path tracer traces what DXC's traces.** `path_trace`, its misses and every material's generated hit group, seven material cases on each API.
  Each image is held to DXC's on the same API with `sv - the path tracer through SGL's binary`, at a relative tolerance of 1e-4.
  The worst pixel is 5.5e-7 relative on DXIL and 2.4e-7 on SPIR-V, which is rounding: about half the pixels differ at all.
- **`slug-traced`'s captures are byte for byte DXC's**, on dx12 and on vulkan, and sr's slug tests pass on both native contexts.
- **sr's whole suite now runs on `dx12-native-dxil` and `vulkan-native-spirv`.**
- **`sgl-real-shaders-test.cc` keeps them covered**: the viewer's shaders, sr's test shaders and the example's, modules and all, past DXC's validator and `spirv-val`.
  52 entry points lower on DXIL and 54 on SPIR-V, and the two left on DXIL are metal's empty closest hit.
- **`path_trace` compiles 12.7 times faster on DXIL and 19 times on SPIR-V.**
  5.8 ms against 73.7 ms, and 5.05 ms against 98.2 ms, front end included, on `release-clang`, best of 40 and of 12.
  The binaries are 29,236 bytes against 28,268, and 63,248 against 42,744, so SPIR-V's is half as large again.
  DXC ran in process at SM 6.8 with its default arguments, which is close to slib's and not the same.

What that found:

- **A host's SGL never reached the native route, or the device.** `compile_source` took neither the context's features nor its route.
  So every hit group the viewer generates went through DXC on a native context, and a compile that asks `has` saw no device.
  `slib::add_context_options` is both now, and hit groups, callables, sv's raster stages and its shade kernels all pass it.
- **`refract`'s eta was splat** into a vector, which `spirv-val` refuses.
- **More than 8 UAVs is a flag** of the module and of `SFI0`, which `RDAT`'s function row leaves out.
- **`RDAT` lists resources by class**, constant buffers and samplers first.
- **A dead block of an earlier `simplify` round** could point past what a later round kept, which only a shader this large reached.

## Hit objects and reordering, measured

On 2026-11-07, DXC 1.9.2602, on the Arc B580.

**The B580 has reordering on both APIs once dx12 runs on the Agility SDK, and micromaps on neither.**

| context | `shader_execution_reordering` | `opacity_micromap` |
|---|---|---|
| vulkan, both routes | yes | no |
| dx12, both routes, system runtime (`SG_HAS_AGILITY` 0) | no | no |
| dx12, both routes, Agility SDK 1.620.1 | yes | no |

Reordering on dx12 is Shader Model 6.9, which only the Agility runtime reports, so it also confirms SM 6.9 on this runtime.
Micromaps are reordering plus raytracing tier 1.2, so the B580's dx12 driver reports a tier below 1.2.
**Both halves are drawn now**, dx12's on Agility 1.620.1 with the debug layer on.

- **What the sweep named, mapped to its entry.** `the type hit_object` was `inspected_primary`, and `the builtin reorder_thread_by_hint` was `reordered_primary`.
  `a resource used as a value` was `hit_reordered_primary`, whose `trace_ray_reordered` reached the generic call path with its structure.
- **All of them lower now.** The sweep with `opacity_micromap`, `shader_execution_reordering` and `vertex_pipeline_stores` lowers 677 entries.
  Every SPIR-V passes `spirv-val` under `--thorough`, and every DXIL the validator.
  What is left of it is metal's fused intersections and empty closest hit, and the gaps no device feature touches.
- **A hit object never moves.** `ir::verify` refuses a phi, a `select`, a store or a composite of one, and a local, global or member holding one.
  It runs after lowering and after every pass.
  The audit: `construct_ssa` promotes locals, and a hit object is never one.
  `flatten_branches` builds a `select` only from a phi, and `share_common_values` takes only `gives_only_a_value`, which `hit_object_read` is not.
  So nothing can create the shape; the verifier is what says so if that changes.
- **The shapes were read back.** `sgl hit object spirv - SGL's own binaries` walks each module word by word as DXC's are walked, on two arms, a read in a branch and a loop.
  It reads DXIL's disassembly for the operation, and for no phi or `select` of one.
  It can fail: a load of the hit object added to SPIR-V's reads failed it, and it was reverted.
- **Both APIs draw alike.** `mixed-route-ray-test.cc` holds a regrouped trace and a hit object through SGL's binary to DXC's within 1e-4.
  That runs on `dx12-hw` and `vulkan`, with the debug layer and validation on.
  That is a native raygen among DXC's stages, and every stage native.
  On dx12 those are SM 6.9 libraries, ours and DXC's, and both create state objects.
  sg's reordering tests pass on `vulkan-native-spirv`.
- **The draw found a DXIL gap the sweep's check missed.** The prelude's `maybe_reorder` wrote `dx::MaybeReorderThread` as HLSL text on dx12, which the DXIL route does not lower.
  It calls `reorder_thread_by_hint` on both APIs now, and the sweep refuses any gap a ray-tracing stage keeps unless it is metal's.
- **SPIR-V's reordering trace is one instruction, DXIL's three**, as each text writes it.

`question-sgl-hit-object-as-parameter` is untouched: today's shapes lower, and none needed the answer.
The crash class of 476f22005 was not reproduced here, as it never was: its report is an RTX's.

## Opacity micromaps, compile-verified only

The B580 has no micromaps, so no draw has run this.
It is held by DXC's output under `--device-feature opacity_micromap`, on 2026-11-07, and by both validators.

- **DXIL needs `dx.op.allocateRayQuery2` (258)**, `(constRayFlags, 1)`, which SM 6.6 refuses, so the module is SM 6.9 and `dx.version` 1.9.
  At 6.7 and above a module with UAVs states resources-may-not-alias, bit 33 of the shader flags, which `Flags must match usage` otherwise refuses.
  `RDAT`'s row names 6.9 in a library that holds a query.
- **SPIR-V needs `RayTracingOpacityMicromapEXT` and `SPV_EXT_opacity_micromap`.** A trace's flags carry `force_omm_2_state`, and no call changes.
- **`program::sees_micromaps` carries it**, so a pipeline stage's trace on such a device states it too.
- **A test fails if this regresses:** it requires the operation, the model and the capability on a compile that also forces 2 states.
- **The skip in `mixed-route-query-test.cc` is gone**, so that test runs on a device with micromaps, which none here is.

**What a device feature pushes into a gap.** The sweep with `opacity_micromap`, `shader_execution_reordering` and `vertex_pipeline_stores` lowers 667 entries, and neither validator refuses one.
Reordering added three gaps here, which the section above closes.

**Left.**

- **Motion traces**, which this issue's first line leaves last.
- **Micromaps** have run on no device: the B580 has them on neither API.
- **`shaped-graphics-test` loses the dx12 device on Agility 1.620.1.** The `dx12-hw` sweep ends in `DXGI_ERROR_DRIVER_INTERNAL_ERROR` about 27 s in.
  The same tree passes all 1958 tests on the system runtime.
  The ray-tracing tests above pass when run alone, and which test removes the device is not known yet.
- **Whether a driver treats a plain export or missing annotations differently.** The B580 did not, and no other GPU ran.
