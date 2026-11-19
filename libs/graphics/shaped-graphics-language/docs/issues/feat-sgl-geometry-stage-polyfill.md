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

2026-11-14: points out, by the compute route: each appended vertex is a primitive, and the vertex half draws a point list.
A mesh stage writes lines or triangles alone, so `sgl describe` reports a pipeline's `geometry_corners`, and the generated definition carries it.
slib then takes the compute route for points, whichever route the context asks for.
`sg - a geometry stage that appends points draws each corner's pixel and no other` holds both route requests to three pixels on an M4.

2026-11-14: strips in, by both routes: a line or triangle strip's window `p` is primitive `p`, an odd triangle's first two corners swapped.
An indexed strip runs `prelude/geometry.sgl`'s `sg_geometry_strip_scan` first, which writes each window's id and parity, or none past a restart, to a new reserved `sg_geometry_strip`.
sg carries the scan on the pipeline, and metal dispatches it outside the pass before the stage, which splits the mesh route's pass for that draw alone.
Four GPU tests on an M4: a triangle strip culled to show each triangle winds alike, an indexed strip with a restart in 16 and 32 bits, the primitive ids past it, and a line strip.

2026-11-17: the strip scan is one workgroup of 256, no longer one thread, with two tree scans over each chunk of 256 windows.
The windows that make a primitive are summed into ids, the restarts' positions folded by max into parities, and both carried to the next chunk.
`geometry-polyfill-benchmark.cc`'s indexed strip, 512 rows of 1024 triangles restarted between rows, on an M4, 10 frames: one draw 595 to 59 ms by the compute route and 576 to 51 ms by the mesh route.
Over 256 draws, 664 to 232 ms and 655 to 229 ms: every indexed strip draw splits the pass for its scan, by either route, so it costs what the compute route's 256 draws do.
`sg - an indexed strip's primitive ids and windings hold past one workgroup of the strip scan` checks every pixel's id over 33 chunks, culled and not; dropping the carried restart fails it.

2026-11-19: webgpu draws it by the compute route, and the 8 tests of `sg - a geometry stage` pass on Dawn; wgpu is unchecked.
Every buffer is bound whole, each view's offset written into `sg_geometry_draw`, and every vertex and index buffer is storage-bindable on webgpu.
SGL's `emitted_source::reserved` names the reserved bindings the text declares, which slib turns into `compiled_shader::geometry_polyfill_streams`.
`pipeline_layout_description::geometry_inputs` then holds only those streams.
Group 3 of such a layout is built per draw, since it names the draw's buffers.
`sg_geometry_strip` is a `mut buffer`, so it is writable in every layout and a draw no scan writes it for binds it a word of its own.
WGSL now writes `@interpolate` only beside a `@location`, which `tag_gs`'s compute half broke.

## Still open

- **dx12 and vulkan draw it**, beside their native stage, so the two images can be compared and the benchmark run: a Windows machine's work.
  The compute half's layout twin carries `geometry_inputs` already; their layouts need the `sg_geometry_*` slots from it, as webgpu's has.
- **wgpu draws it**: the tests ran on Dawn alone, `--runtime deno` needing a deno this machine lacked.
- **The strip scan is one workgroup**: about 4 ms of an M4's frame for half a million windows; a scan across workgroups is next, if a scene needs it.
- **WebGPU's storage buffers**: a compute half reading four streams holds eight with `sg_mesh_output` and the strip, and the debug window makes nine, one past WebGPU's default a stage.
  One stream, as every test has, holds five.
- Whether `vertex_index` on an indexed draw matches D3D's and vulkan's, which differ on the vertex offset.
