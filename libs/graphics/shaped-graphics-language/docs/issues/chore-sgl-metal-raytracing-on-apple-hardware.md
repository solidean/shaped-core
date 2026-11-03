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
- the cost of the pipeline's kernel-then-call shape against inline traces, since metal has no native pipeline object
