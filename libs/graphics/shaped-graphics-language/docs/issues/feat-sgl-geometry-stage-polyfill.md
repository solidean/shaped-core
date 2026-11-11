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
