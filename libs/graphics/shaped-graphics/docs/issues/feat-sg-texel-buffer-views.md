---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/views.md
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/types.hh
---

# Texel buffer views

A format-decoded linear buffer: HLSL `Buffer<T>`, GLSL `samplerBuffer`, Vulkan uniform / storage texel buffers, D3D12 typed SRVs and UAVs.
A texture or a structured buffer covers most cases, which is why it is deferred.
See [concepts/views.md](../concepts/views.md).
