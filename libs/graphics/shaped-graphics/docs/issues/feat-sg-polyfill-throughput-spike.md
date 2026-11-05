---
created: 2026-11-05
priority: next
tags: [webgpu, raytracing]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# Measure how fast the ray-query polyfill traces (S0)

Nothing has measured the webgpu polyfill's speed.
sv's bound raster mode means to trace probe GI through it ([raster-bound.md](../../../shaped-viewer/docs/plans/raster-bound.md#s0-how-fast-the-polyfill-traces)).

- **Scenes**: a coherent mesh, a triangle soup, a generated assembly of instances.
- **Rays**: camera-coherent, probe-like over the sphere, short occlusion rays.
- **Trees**: as built today (primitive order), and sorted by Morton code, prototyped inside the spike to size that fix.
- **Out**: rays per millisecond per scene, ray kind and device, and the pool's size per scene.
- **Form**: a web page beside `sgl-cube`, picking scene and ray kind and showing the rate live, and a nexus benchmark of the same SGL kernels under node with Dawn.
- **Devices**: a laptop RTX 5070, an RTX 5080, a Mac laptop, phones; phones load the page over https on the LAN.

The polyfill exists on webgpu alone ([feat-sg-ray-query-polyfill-dx12-vulkan](feat-sg-ray-query-polyfill-dx12-vulkan.md)), so every number comes through Dawn or a browser.

## Log

**Built**: `examples/graphics/ray-throughput`, run with `uv run dev.py example ray-throughput --preset emscripten-webgpu-release`, and as a page by `graphics-ray-throughput-example-web`.

**Laptop RTX 5070, Dawn under node, 2026-11-05**, the GPU otherwise idle; Mrays/s, best of 3 batches timed on the GPU:

| scene | camera | probe | occlusion |
|---|---:|---:|---:|
| heightfield 262k, row order | 32.1 | 12.0 | 64.9 |
| heightfield 262k, shuffled | 0.04 | 0.04 | 0.06 |
| heightfield 262k, shuffled then Morton-sorted | 44.2 | 31.2 | 89.6 |
| heightfield 1M, row order | 13.8 | 6.2 | 37.5 |
| heightfield 1M, shuffled | 0.01 | 0.01 | 0.02 |
| heightfield 1M, shuffled then Morton-sorted | 21.9 | 25.8 | 79.2 |
| assembly, 2305 instances of 3 parts, 4M triangles placed | 21.4 | 26.8 | 89.6 |

- **The tree's order decides everything**: a shuffled mesh traces 700 to 2500 times slower than the same triangles Morton-sorted.
  Even the row order a generator writes is 2.6 to 4 times slower for probe rays, since its leaves are thin strips.
  So a sorted build is a precondition of probe GI, not an optimization — [perf-sg-polyfill-unsorted-tree](perf-sg-polyfill-unsorted-tree.md).
- **Sorted, probe rays run at about 26 to 31 Mrays/s here**: 262k probe rays a frame would cost about 9 ms, so the budget is nearer 64k a frame.
- **Instancing costs little**: the assembly's TLAS of 2305 instances traces as fast as the sorted single meshes.
- **Two large structures do not fit the 128 MiB pool at once**: a scene's region frees only when its epoch retires, so the example waits for that between scenes.
- **One unbounded dispatch reset a GPU driver.** The first page drew each scene's preview as one dispatch of 1280×720 camera rays.
  On a soup that is about 24 s of work even on the RTX 5070, and in Firefox, on the laptop's integrated AMD GPU, it reset the driver.
  The page now prices each scene with dispatches of 256 rays up, skips a scene too slow for the device, and traces a preview only where it fits 30 ms.
  Probe GI inherits the rule: its rays per dispatch are set from a measured price, never fixed.

**The laptop's integrated AMD GPU, Firefox 159 (wgpu), 2026-11-05**; the page left the adapter's name empty there:

| scene | camera | probe | occlusion |
|---|---:|---:|---:|
| heightfield 262k, row order | 0.25 | 1.30 | 8.51 |
| heightfield 262k, shuffled then Morton-sorted | 5.50 | 2.17 | 10.96 |
| assembly, 2305 instances | 2.12 | 3.91 | 18.14 |
| heightfield 1M, row order | 0.04 | 0.19 | 2.63 |
| heightfield 1M, shuffled then Morton-sorted | 2.52 | 1.96 | 9.51 |
| heightfield 262k and 1M, shuffled | skipped | skipped | 0.01 or skipped |

- **Sorted probe rays run at 2 to 4 Mrays/s here**, 8 to 14 times slower than the RTX 5070 under Dawn.
  64k probe rays a frame would cost 16 to 32 ms, so on this tier live probes update a few thousand rays a frame, or are baked.
- **Order matters even more on a weak GPU**: the row-ordered 1M field's camera rays are 60 times slower than the sorted one's.
- **Even the smallest trial can come near a watchdog**: 256 rays into the 1M soup took 1.2 s.
  Probe GI's first dispatch on an unknown device has to be smaller still, or wait for the sorted build.
- The GPU and the browser changed together, wgpu against Dawn, so the same GPU in Chrome is what separates them.

**The same integrated GPU after the sorted build** ([perf-sg-polyfill-unsorted-tree](perf-sg-polyfill-unsorted-tree.md)), Firefox 159:

| scene | camera | probe | occlusion |
|---|---:|---:|---:|
| heightfield 262k, row order | 8.24 | 2.68 | 16.53 |
| heightfield 262k, shuffled | 8.96 | 4.63 | 16.52 |
| assembly, 2305 instances | 2.14 | 2.51 | 11.19 |
| heightfield 1M, row order | 5.38 | 2.24 | 13.73 |
| heightfield 1M, shuffled | 5.38 | 2.27 | 13.74 |

- **No scene is skipped any more**: the shuffled meshes trace like any other, where before even 256 rays took up to 1.2 s.
- **Probe rays run at 2.2 to 4.6 Mrays/s on this tier**: about 8k a frame fit 2 to 4 ms, so live probes here converge over seconds, or are baked.
- Two readings want a rerun: the first scene's probe rays trail the identical shuffled one, likely clocks still ramping, and the assembly's fell from 3.9 to 2.5.
  The second could be the sorted TLAS ordering 2305 instances worse than their grid order did; the RTX 5070 showed no change.
- Still open: the same page on the RTX 5080, the Mac and the phones, and the native hardware numbers for scale.
