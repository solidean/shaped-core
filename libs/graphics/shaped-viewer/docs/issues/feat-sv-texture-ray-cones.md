---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
  - symbol: sv::generate_material_shader
  - issue: bug-sv-decal-footprint-past-bounce
---

# Every texture is sampled at mip 0

A traced hit has no derivatives, so the generated hit group spells every texture sample with `level = 0.0`.
That is the `explicit_level` of `traced_spelling` in the generator, and it covers both `sv_attribute_<name>` and the `sv_sample_<name>` a fragment resamples through.
**Mip 0 is not what we want in general**: a distant or grazing surface aliases, and reads far more texture memory than its footprint covers.
A triplanar fragment pays it three times, since it samples one texture at three uvs.
The raster pixel stage is unaffected; its derivatives pick the level.

A footprint along the ray (ray cones) is what picks the level.
The cone would also size a decal past a bounce, which is [bug-sv-decal-footprint-past-bounce](bug-sv-decal-footprint-past-bounce.md).
On Intel's Sponza (the `sponza` example) it is what the megakernel's frame is left paying for: about 470 ms at 1920x1080 on an RTX 5070 Laptop GPU, against the wavefront's 150 ms.
BC7 compression already took the scene from 7.7 GB, over the device's budget, to 3.1 GB; the wavefront's frame went from over 800 ms to 150 ms with it.
