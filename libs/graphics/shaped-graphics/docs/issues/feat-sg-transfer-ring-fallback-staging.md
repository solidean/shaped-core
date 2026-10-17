---
created: 2026-10-17
parent: epic-sg-transfer-gaps
tags: [dx12, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_inline.cc
---

# Fallback staging when one list's inline transfers exceed the ring

On dx12 and vulkan the ring blocks on in-flight epochs first, but with nothing in flight it asserts.
webgpu stages the overflow in a one-off buffer and warns, which is the shape to follow.
