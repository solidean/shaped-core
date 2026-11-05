---
created: 2026-11-02
parent: epic-sgl-metal-raytracing-dxr-parity
tags: [metal]
blocked-by: [feat-sg-metal-multiple-tlas-per-dispatch]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
closed: 2026-11-05
resolution: fixed
---

# metal: a pipeline dispatch that traces several TLAS

A metal pipeline finds a closest hit's record from each instance's hit-group offset, in the one buffer sg binds at buffer 5.
So a dispatch whose shaders trace two TLAS finds the second's records through the first's offsets.

sg's side is [feat-sg-metal-multiple-tlas-per-dispatch](../../../../../../shaped-graphics/docs/issues/.closed/2026/11/feat-sg-metal-multiple-tlas-per-dispatch.md): an offsets buffer per bound TLAS.
SGL's side is the emitted trace knowing which TLAS it traced, so it reads that TLAS's offsets.
The TLAS a trace names is known when the module is checked, so the index can be static per trace site.
