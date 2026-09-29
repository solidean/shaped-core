# Tier-1 pipeline tests: the plan

*A plan, not a record: each item is deleted from here in the change that lands it.*

Tier 1 ([testing.md](testing.md#tier-1--backend-agnostic-api-tests-tests)) runs one test body against every backend, and it is where sg's pipeline semantics belong.
`tests/pipeline/` executes them: every value of every fixed-function enum, the draw and dispatch parameters, and the binding semantics.
SGL's pixel-stage rules run there too, as do every texture shape and sampler field and the geometry and tessellation stages.
They run on dx12 and vulkan with validation on, and on webgpu through Dawn under node; metal is yet to be shown running them.
What this file keeps is what is left, and what the tests found that sg or SGL should grow.

## The harness

Every test reads as "set up, draw, expect", which `pipeline_harness.hh` makes true:

- **`draw_offscreen`** takes a size, the target formats and a function that records the draw, and returns the pixels of every target.
  `draw_offscreen_passes` hands the function the command list instead, for a test that opens more than one scope.
- **`rects.hh`'s `rect_batch`** draws rects placed per instance at pixel boundaries, or any list of points a whole-target rect stretches.

A compute test states its dispatch and readback in full: each one is a few lines, so a helper would save little and hide the barrier.

**An oracle passes on any adapter.**
A draw covers whole pixels, a check reads interior pixels only, and a value is chosen exact in its format — rgba8 in steps of 1/255, r32f and rgba16f in powers of two.
Where rasterization rules or precision may differ, the check takes a tolerance and says why.

**One `.sgl` fixture per topic**, beside the ones in `tests/shaders/sgl/`, and `rects.sgl` serves the fixed-function state.
The others are `draws.sgl`, `vertex_input.sgl`, `dispatch.sgl`, `pixel_semantics.sgl`, `sampling.sgl`, `shapes.sgl` and `stages.sgl`.
A pipeline's state is swept through `acquire_raster_pipeline(p, {}, customize)`, so one SGL pipeline serves every value of an enum.

## The enum zoo

**A few large tests that between them draw or dispatch with every value of every enum of sg's pipeline surface.**
Each is designed so that **replacing any one value by another valid one fails the test**, not only exercises the path.
That is the property to design for: a test in which `.less` and `.less_equal` both pass pins nothing about either.

The shape that gives it: one target, a row of small quads, one quad per enum value, each with inputs chosen so that every value of the enum produces a different pixel from the same inputs.
Two values that could agree on the chosen inputs get a second quad with inputs that separate them.

- **The sampler fields no explicit-level probe reaches**: `address_w`, which only a 3D texture reads, and `max_anisotropy` and `mip_lod_bias`, which want a pixel stage's derivatives.

## Binding semantics, executed

- **A storage texture written and sampled within one dispatch or draw** is what WebGPU refuses too, and the portability checks see buffers only.
- A buffer view's size, which SGL cannot observe yet: it has no buffer length, which WGSL's `arrayLength` and HLSL's `GetDimensions` both give.
- Bound state from one rendering scope to the next, which wants a raster pipeline with a binding array; a dispatch's group not leaking into a draw is executed.

## SGL semantics that only a GPU pins

`tests/pipeline/pixel-semantics-test.cc` and `draw-params-test.cc` pin each on dx12, vulkan and webgpu.
What metal still has to be shown doing:

- **A discarded pixel still takes part in its quad's derivatives.**
  WGSL demotes; MSL's `discard_fragment` may end the pixel, and the emulation SGL's TODO describes waits on the test on metal.

## Geometry and tessellation stages

`tests/pipeline/stages-test.cc` runs both stages on dx12 and vulkan, each gated by its feature.
What is left:

- **The partitioning** reaches no pixel the test reads, since a fixed integer factor tiles the same area under all three.

## What moves out of tier 2

Once a semantic runs in tier 1 on every backend, its per-backend copies are deleted: clear, draw and store; depth and stencil; vertex input; base vertex; raster inline constants.
Tier 2 keeps one native-route smoke test per backend — an embedded blob, compute and raster — and the tests of backend internals.
Metal runs tier-1 shaders too, SGL reaching it as MSL through slib's metal edge, so its copies go under the same rule as the others'.

## sg follow-ups this plan found

- **A point list needs its point size written on vulkan without `VK_KHR_maintenance5`, and on metal.**
  sg enables maintenance5 wherever the device has it, which makes an unwritten size 1.0; SGL writes none.
  DXC refuses `[[vk::builtin("PointSize")]]` on an `out` parameter, so SGL would write it as a member of the vertex stage's output struct.
  Until SGL writes one, the point-list tests assume `VK_KHR_maintenance5` on vulkan: `draw-params-test.cc`, `vertex-input-test.cc` and the `point_list` row of `raster-state-test.cc`.
- **`vertex_attribute_format`** has 32-bit components and two 8-bit formats; half floats, 16-bit integers and normalized values, `snorm8x4` and `unorm10_10_10_2` are missing.
  SGL's `@format` takes each case once sg has it (CHK-275).
- **`workgroup_count`** is no stage input of SGL at all, since no backend gives it to a shader; hidden inline constants written per dispatch would.
- **Workgroup memory above 16 KiB** is refused as over the portable limit; a feature for a larger budget, which dx12 and Apple GPUs have, would lift it.
- **Storage writes and atomics in a vertex stage** need a feature, `vertex_stores`, which WebGPU's core lacks.
- **Unbounded binding arrays**, `T[]`, are SGL's spelling and `unsupported-yet`, since sg refuses a binding of count 0 on every backend.
- **A footprint that names an array element**, `albedo[2]: read`, would let a pass skip `declare_array_texture_access` for a constant index.
  SGL's footprint names the whole binding array today, which is correct and asks for the declaration every time.
