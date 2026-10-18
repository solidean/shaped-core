---
created: 2026-10-17
tags: [vulkan, dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_native_scope.hh
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_native_scope.cc
  - symbol: sg::backend::dx12::dx12_native_scope
---

# The native scope is dx12 only

`dx12_native_scope` is the declared escape hatch foreign code records through (see [barriers](../concepts/barriers.md)).
Vulkan has no equivalent, so a vendor denoiser can only run on dx12.
The vulkan shape is the same declaration plus `vkCmdPipelineBarrier2`, and it wants writing when a member needs it.

Two smaller gaps in the dx12 one:

- It declares whole textures rather than subresource ranges.
- A resource the foreign code touches without declaring is caught only by the debug layer.
