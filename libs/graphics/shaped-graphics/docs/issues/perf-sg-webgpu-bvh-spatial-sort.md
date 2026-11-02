---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_acceleration_pool.cc
  - file: libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md
---

# A spatial sort for the webgpu BVH

sg builds the tree over the primitive order, so a triangle soup traces slowly ([raytracing-polyfill.md](../../../shaped-graphics-language/docs/raytracing-polyfill.md#building)).
A proper build sorts primitives along a space-filling curve on the GPU first, then builds by SAH or LBVH.
That wants a GPU sort sg does not have yet.
