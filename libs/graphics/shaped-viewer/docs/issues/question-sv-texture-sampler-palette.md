---
created: 2026-10-17
refs:
  - symbol: sv::sgl_palette_sampler
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_bindings.sgl
---

# A texture samples through a palette, so some sampler states approximate

A hit samples level 0 through one of module `tracer`'s eighteen static samplers, picked per texture by `sv::sgl_palette_sampler`.
The palette tells apart the magnification filter and the address mode of each axis, which is everything a level-0 sample can.
A `mip_lod_bias`, a level clamp, anisotropy and a comparison are dropped, and sv's importers produce none of them.

The alternative is a bindless sampler table indexed from the parameter block, which gives exact states and stops samplers forking permutations at all.
It costs sampler arrays with non-uniform indexing, which webgpu lacks, so it is worth weighing once hits select mips.
