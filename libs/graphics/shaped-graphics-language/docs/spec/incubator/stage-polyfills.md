# Stage Polyfills

*Incubator: not normative.*

## The idea

**A device without a stage can still draw what the stage would have drawn, by running it as a compute pass first.**
The geometry and tessellation stages are gated by `geometry_shader` and `tessellation_shader`, which no webgpu and no metal device grants.
Both stages only turn one set of vertices into another, so the work can move ahead of the draw:
a compute entry point writes the vertices the stage would have produced into a buffer, and an ordinary draw of that buffer follows.

* **The geometry stage** becomes a compute entry point per input primitive, which appends up to `max_vertices` vertices to its own slots of the buffer.
  Strips are cut into lists where `end_strip` is called, and unused slots are degenerate.
* **The tessellation stages** become a compute pass that evaluates the control stage's factors, then a grid of domain locations per patch, then the evaluation stage at each.
  Metal's own tessellation is already shaped this way: a compute kernel writes the factors, and a post-tessellation vertex function evaluates.

**It would be SGL's, not sg's.**
SGL knows both stages' code and can write the compute text; sg would see two pipelines and a buffer between them, which it already knows how to order.
The repository's README names polyfills for ray tracing on webgpu as a goal, and this is the same move for two raster stages.

## What it touches

* Emitting: a compute text per stage, from the same flat tree the stage's own text comes from.
* The pipeline: a polyfilled pipeline is a compute pipeline, a buffer, and a raster pipeline whose vertex stage reads the buffer.
* slib: acquiring a pipeline with a polyfilled stage acquires all three, and a draw records the dispatch ahead of itself.
* The feature: a pipeline that could polyfill no longer needs the feature, so `require geometry_shader` becomes a request rather than a gate.

## Already fixed by the syntax

* Each stage is an entry point of its own, whose inputs and outputs are values: an array of vertices in, a stream or a factors struct out.
  So the compute form reads the same inputs from a buffer and writes the same outputs to one.
* `max_vertices` bounds what the geometry stage appends, which is the buffer's size per primitive.

## Open

* Whether a polyfill is chosen per device, silently, or per pipeline, by the author.
* How a polyfilled tessellation picks its grid when the factors vary per patch: the largest factor's grid, or a prefix sum over patches first.
* Whether a vertex order that differs from the real stage's, within a primitive, is acceptable.
