---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
blocked-by: [feat-sgl-work-graphs]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/gpu-driven-work.md
closed: 2026-11-11
resolution: wontfix
---

# Work graphs polyfilled where a target has none

vulkan, metal and webgpu, and dx12 at SM 6.10 and later.
Candidates, from [gpu-driven-work.md](../../../../spec/incubator/gpu-driven-work.md#polyfills):

- **Level by level**: a record queue per node, a small pass writing indirect dispatch arguments, and a round per graph depth.
  vkd3d-proton emulates D3D12 work graphs on Vulkan this way, which makes it the correctness reference to read first.
- **A persistent megakernel** with atomic queues and a `switch` on the node; no round trips, but no API promises forward progress between workgroups.
- **A raygen with SER**, reordering by node: [question-sgl-raygen-ser-scheduler](../../../question-sgl-raygen-ser-scheduler.md).
- **Work lists**, one per round, where a target has them: [feat-sgl-work-lists](../../../feat-sgl-work-lists.md).

Each is measured against native work graphs on dx12, which decides which is automatic and which opt-in.
Queue sizing is open: a declared bound per node, a host budget, or overflow reported under the `report` profile.

## Decided (2026-11-11): not added

D3D12 has deprecated work graphs, with no support in SM 6.10, so SGL does not add them, natively or polyfilled.
GPU-driven work goes through work lists instead: [feat-sgl-work-lists](../../../feat-sgl-work-lists.md).
The comparison matrix says so below its D table.
