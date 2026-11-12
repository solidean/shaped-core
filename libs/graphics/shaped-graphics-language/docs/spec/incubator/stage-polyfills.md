# Stage Polyfills

*Incubator: not normative.*

## The idea

**Every raster stage SGL has should draw on every target: natively where the target has it, polyfilled where it does not.**
The geometry and tessellation stages are gated by `geometry_shader` and `tessellation_shader`, which no webgpu and no metal device grants.
The mesh and task stages are gated by `mesh_shader`, which no webgpu device grants.

**Metal 4 has no tessellation, so metal takes the polyfill too.**
Metal's tessellator lives on the encoders before Metal 4: `MTL4RenderCommandEncoder` has no `drawPatches`, and `MTL4RenderPipelineDescriptor` no tessellation state.
sg's metal backend is Metal 4 throughout, so reaching the old tessellator would take a second queue and command path for one stage.
The compute tessellator serves webgpu and metal alike, and places points exactly where D3D does, which the old one never promised.

Where a target has no form of the stage, the work can move ahead of the draw:
a compute entry point writes the vertices the stage would have produced into a buffer, and an ordinary draw of that buffer follows.

* **The geometry stage** becomes a compute entry point per input primitive, which appends up to `max_vertices` vertices to its own slots of the buffer.
  Strips are cut into lists where `end_strip` is called, and unused slots are degenerate.
* **The geometry stage on metal could become a mesh stage instead.**
  Metal has mesh shaders, a geometry stage's per-primitive output with a bound is what a mesh workgroup writes, and it stays one draw with no buffer between passes.
* **The tessellation stages on webgpu and metal** become compute passes: the control stage's factors, then a prefix sum of each patch's points and primitives.
  Then D3D's tessellator per patch, and the evaluation stage at each point.
* **The mesh and task stages on webgpu** become a compute pass that runs the task stage, then each mesh workgroup it launches.
  Each mesh workgroup writes fixed slots of a buffer, bounded by its `vertices` and `triangles`, and one indexed draw of the buffer follows.
  The task stage's payload is a buffer slot per launched workgroup.

**A polyfill keeps the primitive order the real stage has.**
The order decides which triangle wins an equal depth and the result of a blend, so a polyfill that loses it draws a different image.
Fixed slots in workgroup order keep it, where an atomic append in completion order would not.

**SGL writes it, and sg draws it, through a documented polyfill protocol.**
SGL knows the stages' code and writes the compute text the protocol asks for.
sg draws a polyfilled pipeline through the call a native one takes, recording the compute ahead of the draw and owning the buffer between them.
The repository's README names polyfills for ray tracing on webgpu as a goal, and this is the same move for the raster stages.
[gpu-driven-work.md](gpu-driven-work.md) is the same move for work lists.

## Automatic or opt-in, by measurement

**Whether a polyfill is automatic or opt-in is decided by measurement, per polyfill and per target.**
Each polyfill is benchmarked against the native stage on a device that has both, dx12 or vulkan, drawing the same scene.
Within a small factor of native, it is automatic, and a pipeline that names the stage builds on a target without it.
Further off, it is opt-in: the program or the host asks for it by name, and without that the feature still refuses.
The benchmark stays in the tree, so a driver that changes the answer reopens the decision.
Every polyfill starts opt-in, until its benchmark has run.
The mesh and task stages' polyfill is the first decided: automatic where the device has no mesh stage, at +17% to +41% over the native stage on a device that has both.

## What it touches

* Emitting: a compute text per stage, from the same flat tree the stage's own text comes from; a mesh text for a geometry stage on metal.
* The pipeline: a polyfilled pipeline is a compute pipeline, a buffer, and a raster pipeline whose vertex stage reads the buffer.
* slib: acquiring a pipeline with a polyfilled stage acquires all three, and a draw records the dispatch ahead of itself.
* sg: a GPU-written indirect count, where the mesh polyfill and the tessellation polyfill need one.
* The feature: a pipeline that could polyfill no longer needs the feature, so `require geometry_shader` becomes a request rather than a gate.

## Already fixed by the syntax

* Each stage is an entry point of its own, whose inputs and outputs are values: an array of vertices in, a stream or a factors struct out.
  So the compute form reads the same inputs from a buffer and writes the same outputs to one.
* `max_vertices` bounds what the geometry stage appends, which is the buffer's size per primitive.
* `@mesh(..., vertices = N, triangles = M)` bounds what a mesh workgroup writes, so the polyfill's buffer has a size per workgroup.

## Open

* What "a small factor of native" is, and whether it is one threshold or one per stage.
