---
created: 2026-10-17
parent: epic-sg-raster-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/resource/buffer.hh
  - symbol: as_vertex_buffer
---

# Validate a bound vertex buffer's element type against the pipeline's vertex input layout

`as_vertex_buffer` conveys only the per-vertex stride (`sizeof(T)`), and does not tie `T` to the pipeline's `vertex_input_layout` — its attribute offsets and formats.
A `vertex_layout<T>` trait that lets binding cross-check the layout, and build it from `T`, is the compile-time shape.
Ahead of it, carrying a type identity of `T` on the view and the expected type on the layout would let binding cross-check them at runtime.
