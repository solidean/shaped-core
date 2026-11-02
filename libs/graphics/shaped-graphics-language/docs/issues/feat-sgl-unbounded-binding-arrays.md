---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
---

# Unbounded binding arrays, and arrays of samplers

A binding array is `T[N]` with `N` a literal or an option ([bindings.md "Binding arrays"](../spec/bindings.md)).
An unbounded `T[]`, an array of samplers and a multi-dimensional array are `unsupported-yet`.

An unbounded array is what a scene of unknown size binds its textures and geometry through: dx12 and vulkan have it, metal through argument buffers, and WebGPU has no binding arrays at all.
sg decides how many slots a group holds, so this has an sg side too.
