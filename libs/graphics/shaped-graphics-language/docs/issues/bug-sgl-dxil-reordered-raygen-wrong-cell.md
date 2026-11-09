---
created: 2026-11-09
refs:
  - test: sg - an SGL raygen that reorders its invocations traces what it would have traced
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
Next: dump the state object description and the shader-table bytes from both dx12 contexts in the failing process and diff them, or take a PIX capture of the failing dispatch.
