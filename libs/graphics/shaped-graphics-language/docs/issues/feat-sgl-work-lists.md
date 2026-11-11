---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
blocked-by: [feat-sg-device-generated-commands]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/gpu-driven-work.md
---

# Work lists: a shader picks the pipeline of each draw or dispatch

D3D12 work lists, announced as the direction after work graphs, extend `ExecuteIndirect` so each record picks its pipeline from a GPU-resident program table.
That is a draw, a compute dispatch or a ray dispatch per record.

The shape has a native answer on three targets:

- **dx12**: work lists, once they ship; not read from a primary source yet.
- **vulkan**: `VK_EXT_device_generated_commands`, whose indirect execution sets switch a pipeline per sequence.
- **metal**: indirect command buffers, whose commands may each set a render or compute pipeline.
- **webgpu**: none, so a polyfill; dx12 needs one too until work lists ship.

The polyfill candidates, each measured against vulkan's native path, are in [gpu-driven-work.md](../spec/incubator/gpu-driven-work.md#polyfills):

- **Level by level**: a record queue per pipeline, a small pass writing indirect dispatch arguments, and one indirect dispatch per pipeline.
- **A persistent megakernel** with atomic queues and a `switch` on the pipeline; no round trips, but no API promises forward progress between workgroups.
- **A raygen with SER**, reordering by pipeline: [question-sgl-raygen-ser-scheduler](question-sgl-raygen-ser-scheduler.md).

Queue sizing is open: a declared bound, a host budget, or overflow reported under the `report` profile.

SGL's side is a program table the host code generator emits, record types a shader writes, and the check that every pipeline a record may name is in the table.
Before designing, read what D3D12 work lists specify; the rest of this issue is from secondary reports.
