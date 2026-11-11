---
created: 2026-11-02
parent: epic-sgl-metal-raytracing-dxr-parity
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
---

# metal: run the ray-tracing pipeline and inline traces on Apple hardware

Before metal can be called at parity, the ray-tracing tests and sv's path tracer should run on Apple silicon.
On 2026-11-02 every pipeline and inline-trace GPU test of sg, sr and sv passed on an M4, which traces in hardware.
That run found and fixed the instance transforms and `accept_and_end_search`; the rest of this list is still open.
Each frame should be compared to dx12's image, with tolerances stated per test.

What a run should cover:

- every pipeline and inline-trace GPU test on an M-series GPU without hardware ray tracing (M1 or M2)
- sv's path tracer on a scene with instances, against the dx12 image

Neither is possible on an M4 alone, checked on 2026-11-11.
The M4 traces in hardware, so it cannot stand in for an M1 or M2.
No dx12 reference image of any traced scene exists in the tree yet, so the comparison needs a Windows run first.
Since feat-sv-deterministic-seed, a backend renders a scene to the same bits every run, but metal and dx12 still differ in traversal and rounding, so the tolerance has to be statistical.

Measured on 2026-11-09 on an M4: the pipeline's kernel-then-call shape costs nothing against an inline trace.
An intersector with `world_space_data` and an intersection table over opaque geometry, calling closest hit and miss through visible function tables,
ran 3.7% faster than an inline `intersection_query` shading the same.
Its motion tags were the cost instead, twice the traversal on a static scene, and are now written only into a package that takes `SOFTWARE_TRACE`.
