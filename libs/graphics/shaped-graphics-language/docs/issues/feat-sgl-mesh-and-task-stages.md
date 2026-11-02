---
created: 2026-11-02
blocked-by: [feat-sg-mesh-task-stages]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# Mesh and task stages

SGL has vertex, pixel, compute, geometry and tessellation stages, and no mesh or task stage.
dx12, vulkan and metal have both; WebGPU has neither.

sg's side is [feat-sg-mesh-task-stages](../../../shaped-graphics/docs/issues/feat-sg-mesh-task-stages.md).
SGL's side is the two stages, their outputs, and a feature that refuses them on webgpu.
A compute polyfill is the incubator's idea for the geometry stages there ([stage-polyfills.md](../spec/incubator/stage-polyfills.md)), and could serve these too.
