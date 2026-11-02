---
created: 2026-10-17
priority: someday
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/resource/index_buffer_view.hh
  - symbol: sg::index_buffer_offset_alignment
---

# An odd first index into a 16-bit index buffer is refused everywhere

`sg::index_buffer_offset_alignment` is a portable rule every backend asserts, so the failure is the same on all of them rather than metal-only.
See [concepts/raster-pipeline.md](../concepts/raster-pipeline.md).
What it costs is real: a sub-mesh whose first index happens to be odd is a legal D3D12 and Vulkan draw that sg rejects, so a caller pads the range or uses 32-bit indices.

Lifting it means staging a shifted copy of the index range, which needs a copy the render pass it sits inside cannot record.
So it wants a pre-pass fixup or an aligned index allocator, and neither is worth building before something hits it.
