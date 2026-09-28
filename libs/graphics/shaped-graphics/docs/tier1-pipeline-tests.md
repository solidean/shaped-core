# Tier-1 pipeline tests: the plan

*A plan, not a record: each item is deleted from here in the change that lands it.*

Tier 1 ([testing.md](testing.md#tier-1--backend-agnostic-api-tests-tests)) runs one test body against every backend, and it is where sg's pipeline semantics belong.
Today it executes a thin slice of them: SGL shaders double a buffer, bind groups by position, sample a few textures, and draw instanced and indexed quads.
`compute/compute-sync-test.cc` runs workgroup memory, barriers, atomics and a binding array read through a `nonuniform` index.
The fixed-function state, the draw parameters and most binding semantics are validated but never executed on a GPU.

SGL can now say what these tests need: stage inputs, integer and bit arithmetic, `discard`, depth and sample-mask output, interpolation, vertex formats,
every texture shape, barriers, workgroup memory, atomics and the maths builtins.
So what is left is the tests themselves.

## The harness

Every test below reads as "set up, draw, expect", which two helpers make true:

- **`draw_offscreen`** takes a size, the target formats and a function that records the draw, and returns the pixels of every target.
- **`dispatch_and_read`** takes a dispatch and returns a buffer's contents as typed values.

Without them each test repeats the forty lines of setup `raster-test.cc` carries now.

**An oracle passes on any adapter.**
A draw covers whole pixels, a check reads interior pixels only, and a value is chosen exact in its format — rgba8 in steps of 1/255, r32f and rgba16f in powers of two.
Where rasterization rules or precision may differ, the check takes a tolerance and says why.

**One `.sgl` fixture per topic**, beside the ones in `tests/shaders/sgl/`.
`rects.sgl` serves the fixed-function state: rects placed per instance at pixel boundaries, each with a depth and a color.
`vertex_input.sgl`, `dispatch.sgl` and `bindings.sgl` are still to come.
A pipeline's state is swept through `acquire_raster_pipeline(p, {}, customize)`, so one SGL pipeline serves every value of an enum.

## The enum zoo

**A few large tests that between them draw or dispatch with every value of every enum of sg's pipeline surface.**
Each is designed so that **replacing any one value by another valid one fails the test**, not only exercises the path.
That is the property to design for: a test in which `.less` and `.less_equal` both pass pins nothing about either.

The shape that gives it: one target, a row of small quads, one quad per enum value, each with inputs chosen so that every value of the enum produces a different pixel from the same inputs.
Two values that could agree on the chosen inputs get a second quad with inputs that separate them.

- **`vertex_attribute_format`**: one attribute per format, each fed bytes whose decoded value is distinct from what any other format would decode them to — the normalized formats included.
- **Texture view dimensions**: each shape sampled at one texel whose value encodes its layer, face or slice, so a wrong dimension reads a wrong value.
- **Sampler address and filter modes**: samples outside 0..1 where repeat, mirror and clamp differ, and at half a texel where nearest and linear differ.
  Comparison samplers get every `compare_op` again, as the depth test does.

**Default and `--thorough`.**
Each test runs every value by default when it is one draw per value into one target.
A combinatorial sweep — every blend factor against every op — narrows to a covering subset under `!nx::is_thorough()`.

## Draw and dispatch parameters

- `dispatch_threads` against `dispatch_groups`, with a partial last group: every thread writes its `@thread_id`, and one workgroup counts its threads with an atomic in workgroup memory.
- Inline constants that change per draw and per dispatch.

## Binding semantics, executed

- A buffer view with an offset and a size: the shader sees exactly that range.
- `int`, `uint` and `float4` element buffers.
- Transient, persistent and staging groups, each driving the same pipeline.
- A read-only and a read-write view of one buffer.
- Two to four groups at once, rebound between dispatches, and one group shared by two pipelines of one layout.
- Bound state across `render_to` scopes: a group bound in one scope is not bound in the next, and a compute-bound group does not leak into a draw.

## SGL semantics that only a GPU pins

What SGL states for every target, and each backend has to be shown to do:

- **`@vertex_index` and `@instance_index` include the base**, on dx12 through `SV_StartVertexLocation` and `SV_StartInstanceLocation`.
  Those need shader model 6.8, which DXC 1.9 confirms for DXIL and SPIR-V alike; slib compiles at 6.8 by default.
  With slib's `-fvk-support-nonzero-base-*` flags the SPIR-V folds to `VertexIndex` and `InstanceIndex`, as intended — at compile level; the draw test is what pins it.
- **A discarded pixel still takes part in its quad's derivatives**: alpha-tested foliage, then a sample in the same quad.
  WGSL demotes; MSL's `discard_fragment` may end the pixel, and the emulation SGL's TODO describes waits on this test.
- **Flat interpolation takes the first vertex** of the primitive, on every backend.
- **Depth output**, plain and conservative, against the depth test; **sample mask output** against a multisampled target.
- **`gather`'s texel order**, the same four texels in the same order on every backend.

## Geometry and tessellation stages

SGL writes both stages for dx12 and vulkan, and DXC accepts the text for DXIL and SPIR-V; no draw has run them yet.
Each test below is gated by its feature, so an adapter without it skips rather than fails.

- **Tessellation**: a triangle patch with fixed factors, whose evaluation stage writes the domain location as a colour.
  The covered pixels and one interior colour pin the domain, the partitioning and the winding the control stage names.
- **A factor of zero culls the patch**, which dx12 and vulkan agree on.
- **Geometry**: a stage that emits each triangle twice, offset, then ends the strip, so the second copy is drawn and no bridging triangle is.
- **`@primitive_id`** read by the geometry stage and passed on flat, one value per triangle.
- **A pipeline without the feature is refused** by `create_raster_pipeline` naming it, which `uncached.cc` does now whatever language the shader is in.

## What moves out of tier 2

Once a semantic runs in tier 1 on every backend, its per-backend copies are deleted: clear, draw and store; depth and stencil; vertex input; base vertex; raster inline constants.
Tier 2 keeps one native-route smoke test per backend — an embedded blob, compute and raster — and the tests of backend internals.
**Metal's copies stay** until metal runs tier-1 shaders, which the SGL-to-MSL-to-`newLibraryWithSource` path in flight is what brings.

## sg follow-ups this plan found

- **`blend_factor` has no constant factor**, so `set_blend_constants` has no observable effect and the blend test cannot pin it.
  `constant` and `one_minus_constant` exist on every backend.
- **A point list needs its point size written on vulkan without `VK_KHR_maintenance5`, and on metal.**
  sg enables maintenance5 wherever the device has it, which makes an unwritten size 1.0; SGL writes none.
  DXC refuses `[[vk::builtin("PointSize")]]` on an `out` parameter, so SGL would write it as a member of the vertex stage's output struct.

- **`vertex_attribute_format`** has 32-bit components and two 8-bit formats; half floats, 16-bit integers and normalized values, `snorm8x4` and `unorm10_10_10_2` are missing.
  SGL's `@format` takes each case once sg has it (CHK-275).
- **`workgroup_count`** is no stage input of SGL at all, since no backend gives it to a shader; hidden inline constants written per dispatch would.
- **Workgroup memory above 16 KiB** is refused as over the portable limit; a feature for a larger budget, which dx12 and Apple GPUs have, would lift it.
- **Storage writes and atomics in a vertex stage** need a feature, `vertex_stores`, which WebGPU's core lacks.
- **Unbounded binding arrays**, `T[]`, are SGL's spelling and `unsupported-yet`, since sg refuses a binding of count 0 on every backend.
- **A footprint that names an array element**, `albedo[2]: read`, would let a pass skip `declare_array_texture_access` for a constant index.
  SGL's footprint names the whole binding array today, which is correct and asks for the declaration every time.
