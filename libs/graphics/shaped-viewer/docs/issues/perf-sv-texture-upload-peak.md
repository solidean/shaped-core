---
created: 2026-10-28
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - symbol: sv::gpu_resource_manager::_queue_compression
---

# Every texture lands uncompressed before any is compressed, so Sponza peaks at 6.5 GB and spills

**A texture uploads as RGBA8 with its whole chain, and is compressed to BC7 only once that chain is complete.**
Compression is spread over frames by `gpu_resource_manager`'s pending work, but nothing bounds how many uncompressed textures exist at once.
So a scene's every texture is resident uncompressed before the encodes catch up.

Measured on Intel's Sponza, a laptop RTX 5070 with an 8 GB card and a 7.1 GB budget, on 2026-10-28:

- 72 PNG textures, 1.21 billion texels: 6.00 GiB as RGBA8 with mips, 1.50 GiB as BC7.
- The process holds 6.5 GB while Sponza uploads, and about 3 GB once warm.
- Windows demotes allocations to system memory at that peak, and does not promote them back promptly.
  With dlss_rr's own resources created on top, 2.2 GB of the process's allocations stayed in shared system memory, against 0.38 GB with svgf.
- Every pass then reads over PCIe: the hybrid's raster and lighting run 4 to 8 times slower, and a 10.8 ms frame becomes 35 to 46 ms.
  `hybrid-benchmark.cc` is the measurement, and docs/plans/hybrid-renderer.md records it.

**Wanted: a bound on the uncompressed bytes waiting for compression**, a few hundred MB, with further uploads held on the CPU until compressed ones free their room.
The peak is then the compressed working set plus that budget.
The other shape is streaming each texture into transient memory and encoding it straight into its persistent BC7 texture; a larger change for a similar peak.
