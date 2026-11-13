---
created: 2026-11-05
priority: next
tags: [webgpu, raytracing]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_acceleration_pool.cc
  - issue: feat-sg-polyfill-throughput-spike
---

# The polyfill's BLAS follows the primitive order, which makes an unordered mesh trace up to 2500 times slower

The polyfill pairs consecutive primitives into leaves and consecutive nodes into levels, with no spatial sort.
[raytracing-polyfill.md](../../../shaped-graphics-language/docs/raytracing-polyfill.md#building) is that build.
S0 measured what it costs ([feat-sg-polyfill-throughput-spike](feat-sg-polyfill-throughput-spike.md#log)).
A shuffled 1M-triangle mesh traces at 0.01 Mrays/s, and the same mesh Morton-sorted at 22 to 79.
Real assets, CAD exports above all, are not stored in spatial order, so probe GI on webgpu waits on this.

Morton-sorting the primitives before the existing build is what S0 measured, done there on the CPU.
Where it runs — the CPU at build, or a GPU radix sort the build kernels already sit beside — and whether a better split than Morton (an LBVH with SAH refinement) pays, are this issue's to decide.
The sort permutes primitives, so a hit must still report the primitive index the caller's geometry has.

## Log

**Built 2026-11-05**: a GPU radix sort of the records by the Morton code of their box centers, before the tree is boxed, for a BLAS of triangles or boxes and for a TLAS.
Plain WGSL with no subgroups and no float atomics: blocks walked serially by one invocation each keep it stable, and the bounds reduce through integer atomics on order-preserving bits.
`fast_build`, fewer than 16 records, or a scratch past one 128 MiB binding keep the caller's order.
`sg webgpu - a build of many triangles sorts them by the Morton code of their centers, …` tests the layout: each record once, none out of order.
`sg - an inline trace through a mesh built in shuffled order reports each triangle's own index` tests the hits, on every backend.

**Re-measured on the laptop RTX 5070, Dawn under node**, probe rays in Mrays/s: the 1M shuffled mesh from 0.01 to 30.2, the 262k one from 0.04 to 38.1.
Every order of a mesh now traces alike, and the generator's row order gained 3 to 5 times as well, since its leaves were thin strips.
