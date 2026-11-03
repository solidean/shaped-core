---
created: 2026-10-17
parent: epic-sg-raster-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/command_list
  - symbol: dispatch_groups_indirect
---

# Indirect draws

**Indexed indirect draws landed** as `draw_indexed_indirect(args, first, count)`, with a count the CPU knows.
One `ExecuteIndirect` on dx12; one draw per record on vulkan, metal and webgpu.

Still open:

- **A GPU-written count**, so a culling pass can compact what it keeps.
  dx12's `ExecuteIndirect` and vulkan 1.2's `vkCmdDrawIndexedIndirectCount` have it natively; metal needs indirect command buffers, and webgpu has none.
- **One vulkan call for many records**, which needs the device created with `multiDrawIndirect`.
- **Non-indexed `draw_indirect`**, which nothing has asked for yet.
