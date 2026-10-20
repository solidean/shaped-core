---
created: 2026-10-17
parent: epic-sg-webgpu-gaps
tags: [webgpu]
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu
---

# webgpu image views ignore `depth_slice_range`

WebGPU cannot express a depth-slice range on a view, so the field is dropped.
Whether that should be refused instead of ignored is open.
