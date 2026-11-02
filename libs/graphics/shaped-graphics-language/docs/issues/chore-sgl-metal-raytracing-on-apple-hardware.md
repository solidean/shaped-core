---
created: 2026-11-02
parent: epic-sgl-metal-raytracing-dxr-parity
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
---

# metal: run the ray-tracing pipeline and inline traces on Apple hardware

The metal pipeline is exercised by CI alone, and the spec's parity gaps were found by reading, not by running.
Before metal can be called at parity, the ray-tracing tests and sv's path tracer should run on Apple silicon.
Each frame should be compared to dx12's image, with tolerances stated per test.

What a run should cover:

- every pipeline and inline-trace GPU test, on at least one M-series GPU with hardware ray tracing and one without
- sv's path tracer on a scene with instances, against the dx12 image
- the cost of the pipeline's kernel-then-call shape against inline traces, since metal has no native pipeline object

[chore-sgl-msl-memory-ordering-on-a-mac](chore-sgl-msl-memory-ordering-on-a-mac.md) is the same kind of run for memory ordering, and can share the setup.
