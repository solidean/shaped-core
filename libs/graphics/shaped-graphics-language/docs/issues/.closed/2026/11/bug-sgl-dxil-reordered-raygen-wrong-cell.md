---
created: 2026-11-09
refs:
  - test: sg - an SGL raygen that reorders its invocations traces what it would have traced
closed: 2026-11-09
resolution: fixed
---

# A reordering raygen runs nothing on one of the two dx12 contexts of a test process

What is wrong or wanted, and what is already known.

On the dx12 native-DXIL backend, `reordered_path` of `raytracing_pipeline.sgl` leaves every cell empty: the first wrong one reads `cell (0, 0): hit 0, instance 0, primitive 0, lit 0, t 0, u 0, v 0`.
Seen on 2026-11-09 on an RTX 5070 Ti, at `ecd4191e9`, deterministic over reruns; the dx12 route through DXC and vulkan pass the same test.
Hypothesis only: the native writer's lowering of `reorder` or of the hit object's invoke, which `35f39d1dd` (EMIT-246) brought to both writers.

Narrowed on 2026-11-09: it is the free `maybe_reorder(hint, bits)` alone.
Without that line `reordered_primary` traces every cell right on native DXIL, and only the test's own feature check fails, as it should.
The native writer lowers it as `MaybeReorderThread` (268) of `HitObject_MakeNop` (266), the opcodes DXC's own lowering uses, and the validator accepts the container.
A reorder of a traced hit object (`inspected_primary`) and `trace(…, reorder = …)` pass on the same route, so `MakeNop` is the one difference.
Next: DXC's container for the same raygen beside ours, disassembled, around the reorder.

Measured again on 2026-11-09, evening, which overturns the route theory:

* Each run fails on exactly one of the two dx12 contexts, never both and never vulkan: native DXIL in some runs, dx12-hw through DXC in others.
* `SC_BLOB_CACHE=off` changes nothing, so no stale persistent entry is behind it.
* The failing context's raygen writes nothing at all: with the ids buffer prefilled with 7, every cell still reads 7, and a second readback after `idle_completion` agrees.
* A standalone D3D12 harness builds the same pipeline from the same HLSL, one `lib_6_9` library per entry, with the test's scene, tables and checks.
  It passes every time: `-O3` or `-Od`, with or without `-Zi`, `-Gis` and `-enable-16bit-types`, with the debug layer, and on two or four devices of one process at once.
  The harness is in a session scratchpad, not in the tree; it is the starting point for `docs/bugs-external` if the driver turns out to be at fault.

So the pipeline and the shaders are not enough to trigger it.
What sg adds is: its root signature and descriptor heaps, its shader-table layout, the second context sharing the one D3D12 device, and the other tests running on it in parallel.

Narrowed further the same evening:

* It needs both dx12 contexts: with either one skipped, the other passes every run.
* Both contexts share one `ID3D12Device`, and the context whose state object is created second is the one that fails, whichever route that is.
  The two creations do not overlap; they are tens of milliseconds apart.
* The standalone harness had made a device per pipeline, since the Agility SDK's device factory hands out a new one per `CreateDevice`.
  On one shared device it fails too, deterministically: a DXC pipeline and an SGL-native one, in either order, and the second gets 10 of 64 cells wrong.
  Its symptom differs from sg's: instance 20's shadow ray answers unlit, where sg's raygen writes nothing.
  Two DXC pipelines pass, `-O3` beside `-Od` included, and so do two native ones.
* One pipeline that mixes the routes fails on its own device: DXC's reordering raygen and misses beside SGL's native `shade`, with the same 10 cells.
  With DXC's non-reordering `primary` raygen it passes.
* Neither the container digest (signing SGL's containers with DXC's validator), nor the export names, nor the payload struct names change any of it.
* SGL's native DXIL carries no `!dx.dxrPayloadAnnotations`, which DXC writes for every `[raypayload]` struct.
  [feat-sgl-native-ray-tracing](../../../feat-sgl-native-ray-tracing.md) leaves it out on purpose, since the validator and state object creation accept a module without it.
  Hypothesis: NVIDIA's driver lays a payload out by its access qualifiers, so an annotated shader and an unannotated one disagree on where a field lives.
  The shared-device failure would then mean the driver keeps something of that layout per device.


Settled the same night: it is NVIDIA's driver, and the annotations are the trigger.
[nvidia-reorder-payload-qualifiers-across-pipelines](../../../../../../../../docs/bugs-external/nvidia-reorder-payload-qualifiers-across-pipelines/readme.md) reproduces it.
It takes 40 lines of HLSL and stock DXC.
Two pipelines on one device, one compiled with payload access qualifiers and one with `-disable-payload-qualifiers`, raygens that reorder: the second loses its closest hit's payload write.
In sg, compiling the DXC route with `-disable-payload-qualifiers` too made this test pass on both contexts, five runs of five.

Resolved by SGL's native writer stating every payload's access in `!dx.dxrPayloadAnnotations`, as DXC does for the text (EMIT-137).
This test then passes on both dx12 contexts, five runs of five, and `slib - SGL's DXIL states each payload field's access, as DXC does for SGL's text` holds the annotations.
