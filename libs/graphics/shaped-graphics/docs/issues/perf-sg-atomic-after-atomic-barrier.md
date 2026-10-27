---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/barrier/resource_access.hh
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_barrier.cc
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_barrier.cc
  - symbol: sg::access_flag::shader_atomic
---

# An atomic has its own access flag and nothing reads it differently yet

`shader_atomic` orders as a read and a write, which is what both D3D12 and Vulkan formally want between two atomic dispatches.
Letting atomic-after-atomic run free is a tracker change once something sets the flag.
SGL has atomics now (`atomic[uint]`), but the footprint does not set `shader_atomic` yet; only the backends' barrier mapping reads it.
