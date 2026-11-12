---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/gpu-driven-work.md
  - file: libs/graphics/shaped-graphics-language/docs/comparison/matrix.md
---

# Every stage and GPU-driven work on every target: native where supported, polyfilled where not

Tessellation, geometry, mesh and task stages, and work lists, on dx12, vulkan, metal and webgpu.
Each one is native where the target has it, and polyfilled where it does not.
The stages are row D6 of the [comparison matrix](../comparison/matrix.md), ⚠️ today for the geometry and tessellation polyfills; work lists are row D9.

**Work graphs are not added, natively or polyfilled**: D3D12 has deprecated them, with no support in SM 6.10.
GPU-driven work goes through work lists instead.

**Whether a polyfill is automatic or opt-in is decided by measurement.**
Each polyfill lands with a benchmark against the native path on a device that has both.
Within a small factor, it is automatic and its feature stops refusing; further off, a program or a host asks for it by name.
[stage-polyfills.md](../spec/incubator/stage-polyfills.md#automatic-or-opt-in-by-measurement) states the rule.

**Polyfills are open to creative routes**, not only a compute pass ahead of a draw.
A geometry stage may become a mesh stage on metal, and divergent compute or a work list's records may run as a raygen that reorders with SER.
[gpu-driven-work.md](../spec/incubator/gpu-driven-work.md) lists the candidates.

Children, native first where there is a native:

- [feat-sgl-geometry-stage-polyfill](feat-sgl-geometry-stage-polyfill.md): metal and webgpu.
- [feat-sgl-tessellation-polyfill](feat-sgl-tessellation-polyfill.md): metal and webgpu.
- [feat-sgl-mesh-task-polyfill](feat-sgl-mesh-task-polyfill.md): webgpu.
- [feat-sgl-work-lists](feat-sgl-work-lists.md): native on three targets, on sg's [feat-sg-device-generated-commands](../../../shaped-graphics/docs/issues/feat-sg-device-generated-commands.md).
- Closed as obsolete, since Metal 4 has no tessellation: [feat-sgl-metal-tessellation](.closed/2026/11/feat-sgl-metal-tessellation.md).
- Closed as wontfix: [feat-sgl-work-graphs](.closed/2026/11/feat-sgl-work-graphs.md) and [feat-sgl-work-graph-polyfill](.closed/2026/11/feat-sgl-work-graph-polyfill.md).
- [question-sgl-raygen-ser-scheduler](question-sgl-raygen-ser-scheduler.md): whether a raygen with SER beats compute for divergent work.
