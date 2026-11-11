---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - issue: feat-sg-indirect-draws
---

# Device-generated commands: a GPU-written list of draws and dispatches, each choosing its pipeline

The host side of work lists, [feat-sgl-work-lists](../../../shaped-graphics-language/docs/issues/feat-sgl-work-lists.md).
A program table of pipelines, a buffer of records a shader writes, and one command that executes them, each record naming its pipeline.

- **dx12**: D3D12 work lists once they ship; `ExecuteIndirect` with a GPU-written count until then, which switches no pipeline.
- **vulkan**: `VK_EXT_device_generated_commands` with indirect execution sets.
- **metal**: indirect command buffers with per-command pipeline state.
- **webgpu**: none.

A GPU-written count alone, which [feat-sg-indirect-draws](feat-sg-indirect-draws.md) leaves open, is the first step on all three.
