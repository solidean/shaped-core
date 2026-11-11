---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-11T18:02Z
---

# The geometry stage on metal and webgpu, polyfilled

Neither metal nor webgpu has a geometry stage, and SGL refuses one there.

Two routes, which [stage-polyfills.md](../spec/incubator/stage-polyfills.md) describes:

- **A compute pass ahead of the draw**, appending each primitive's vertices to its own slots of a buffer; reaches both targets.
- **A mesh stage on metal**, which has mesh shaders: the geometry stage's bounded output per primitive is what a mesh workgroup writes, in one draw.

Each route is measured against dx12's native geometry stage on the same scene, which decides whether it is automatic or opt-in.

## Decided (2026-11-11)

- **The compute route first**, on the mesh polyfill's words and vertex half; metal's mesh route later, measured against it.
- **The vertex layout is the shader's**: a `@vertex struct` stream is packed a word a leaf, so the compute half reads the buffers as words, and only their offsets are the draw's.

## Log

- **2026-11-11: the SGL half.**
  `sgl emit --mesh compute --vertex <vs>` writes the vertex stage and the geometry stage as one compute entry point, and `--mesh vertex` the mesh polyfill's vertex half, CHK-525.
  Lists only, triangles and lines; points, strips in, and `view_index` are refused by name.
  `vertex_index` on an indexed draw is the index read, without the vertex offset: a guess at D3D's, to check against dx12 drawing the same.
