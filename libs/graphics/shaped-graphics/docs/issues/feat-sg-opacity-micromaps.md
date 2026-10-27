---
created: 2026-10-17
parent: epic-sg-acceleration-structure-gaps
---

# Opacity micromaps

Behind an sg feature: per-micro-triangle opaque / transparent / unknown masks built with the BLAS.
Most candidate hits on alpha-tested geometry then never reach an any-hit decision.
DXR 1.2 and vulkan's `VK_EXT_opacity_micromap` expose them to ray queries too; metal's equivalent is unchecked.
The planned caller is shaped-viewer's wavefront tracer, which keeps cutout opacity as data so it can bake them — [wavefront.md](../../../shaped-viewer/docs/wavefront.md).
