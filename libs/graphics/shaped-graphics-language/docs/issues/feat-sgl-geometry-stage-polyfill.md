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
- **2026-11-11: drawn on metal.**
  `ctx.set_geometry_polyfill(...)` opts in, slib acquires both halves, and sg's metal backend dispatches the compute half outside the pass and draws the slots back.
  `tests/pipeline/stages-test.cc`'s geometry tests pass on an Apple M4 through it: ended strips, primitive ids, and indexed draws of 16-bit and 32-bit indices with a vertex offset.
- **2026-11-12: the mesh route on metal.**
  `sgl emit --mesh mesh --vertex <vs>` writes the vertex and geometry stages as one mesh entry point, a primitive a lane.
  slib builds a mesh pipeline of it where `ctx.geometry_polyfill_route()` is `mesh`.
  sg draws it in the pass as one mesh draw; the geometry tests draw the same images by both routes on an M4.
  `geometry-polyfill-benchmark.cc` measured it faster in every scene: a third in one draw, and 17 to 27 times over 256 draws, each splitting the compute route's pass.
  So `mesh` is the default route; triangles out only, since a line has no filler for a slot a lane leaves.

2026-11-13: lines out by the mesh route, with a filler vertex per workgroup outside the clip volume that an unwritten line joins to itself.
The workgroup keeps room for it, `max_vertices = lanes * V + 1`, and the mesh function is a line mesh, so slib draws a line list.
`sg - a geometry stage that appends lines draws each triangle's outline and nothing inside it` holds both routes to an outline on an M4.

## Still open

- **dx12 and vulkan draw it**, beside their native stage, so the two images can be compared and the benchmark run: a Windows machine's work.
  The compute half's layout twin needs the `sg_geometry_*` slots there, as the mesh polyfill's holds `sg_mesh_output`.
- **webgpu draws it**, which needs `buffer_usage::readonly_buffer` on a vertex and an index buffer a polyfilled pipeline reads, since WebGPU binds no other as storage.
- **Strip topologies in**, whose primitives share corners, and points in and out.
- Whether `vertex_index` on an indexed draw matches D3D's and vulkan's, which differ on the vertex offset.
