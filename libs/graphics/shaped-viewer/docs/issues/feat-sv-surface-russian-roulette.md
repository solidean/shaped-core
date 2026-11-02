---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/pt_sampling.sgl
---

# Surface bounces have no Russian roulette, and `max_bounces` is a hard cut

A scattering walk inside a medium ends by Russian roulette, after 16 events, on the throughput's largest channel; `pt.scatter_cap` (4096) is the guard behind it.
That bought cost rather than correctness: the same subsurface capture runs in about 8-12 seconds against 35 for the old 256-event cap.
The surface bounces still have no roulette, and `max_bounces` is still a hard cut.
