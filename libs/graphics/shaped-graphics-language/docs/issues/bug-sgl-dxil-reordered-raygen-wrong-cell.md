---
created: 2026-11-09
refs:
  - test: sg - an SGL raygen that reorders its invocations traces what it would have traced
---

# The native DXIL route's reordering raygen traces nothing

What is wrong or wanted, and what is already known.

On the dx12 native-DXIL backend, `reordered_path` of `raytracing_pipeline.sgl` leaves every cell empty: the first wrong one reads `cell (0, 0): hit 0, instance 0, primitive 0, lit 0, t 0, u 0, v 0`.
Seen on 2026-11-09 on an RTX 5070 Ti, at `ecd4191e9`, deterministic over reruns; the dx12 route through DXC and vulkan pass the same test.
Hypothesis only: the native writer's lowering of `reorder` or of the hit object's invoke, which `35f39d1dd` (EMIT-246) brought to both writers.

Narrowed on 2026-11-09: it is the free `maybe_reorder(hint, bits)` alone.
Without that line `reordered_primary` traces every cell right on native DXIL, and only the test's own feature check fails, as it should.
The native writer lowers it as `MaybeReorderThread` (268) of `HitObject_MakeNop` (266), the opcodes DXC's own lowering uses, and the validator accepts the container.
A reorder of a traced hit object (`inspected_primary`) and `trace(…, reorder = …)` pass on the same route, so `MakeNop` is the one difference.
Next: DXC's container for the same raygen beside ours, disassembled, around the reorder.
