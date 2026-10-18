---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/restir-gi.md
  - file: libs/graphics/shaped-viewer/docs/plans/rendering-roadmap.md
---

# ReSTIR GI, for the interactive hybrid mode

[plans/restir-gi.md](../plans/restir-gi.md) is the plan.
The hybrid rasterizes primary visibility and lights it by tracing; resampling each pixel's paths across neighbors and frames is what makes that lighting clean enough per frame.
It is for the interactive mode alone: a final render converges with the wavefront tracer and no reuse, so nothing it resamples biases a finished image.
It was chosen over a radiance cache, which stays a possible later complement.
ReSTIR DI runs beside it as a second track, after power-weighted light selection; the plan's Decisions section records what the user settled.
The plan goes to the rendering-modes review in [the roadmap](../plans/rendering-roadmap.md), together with the hybrid's visibility buffer.
