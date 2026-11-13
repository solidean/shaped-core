---
created: 2026-11-11
parent: epic-sv-raster-path
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/material_runtime.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
  - symbol: parallax_velocity
---

# Parallax occlusion mapping for raster primaries

`sv_parallax` runs at every traced hit, in the megakernel and the wavefront tracer.
The raster paths do not march it yet: `raster_shade`, the hybrid G-buffer's primaries, and the raster preset.
A hybrid frame therefore shows its primaries unshifted while its secondaries start at the shifted point.

The march itself is the same, `parallax_velocity` from the triangle's uvs and corners, so raster needs the triangle's edges and uvs at the fragment.
The G-buffer has to store the offset uv, or the shifted context, so the traced secondaries start where the primary was shaded.
