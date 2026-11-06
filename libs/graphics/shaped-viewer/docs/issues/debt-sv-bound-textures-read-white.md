---
created: 2026-11-06
tags: [webgpu, raster]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
  - issue: feat-sv-raster-bound-textures-and-assemblies
---

# The bound spelling reads every texture as a white texel

The bound raster mode binds no textures before C1, so the generator's `bound` spelling samples none.
A texture attribute reads `float4(1.0)` through its own swizzle and its sample transform's scale and bias, which the parameter block carries.
A map scaled by a factor then shows its factor, and a map with an identity transform shows white.
C1's per-material texture group replaces it ([feat-sv-raster-bound-textures-and-assemblies](feat-sv-raster-bound-textures-and-assemblies.md)).
