---
created: 2026-10-18
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/emissive-triangles.md
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/impl/emitters.hh
---

# An emissive mesh is sampled by area, whatever its emission

A type declaring its emission makes an instance a light, sampled by area and balanced against the bounce ray in both tracers ([lights.md](../lights.md#emissive-instances)).
A mostly dark textured or per-vertex emitter spends most of its samples where it emits nothing.
A power-weighted triangle CDF is the fix, under Later in [plans/emissive-triangles.md](../plans/emissive-triangles.md).

`sv::summarize_texels` weighs a block-compressed level by the mean of its decoded blocks, but no acquire supplies one yet: the texture manager uploads uncompressed levels alone.
Bistro's FBX import decodes each texture to rgba8 mip 0, and the manager's later BC7 encoding keeps the summary of those source pixels, made inexact.
A raster layer draws no emitter's light yet.
