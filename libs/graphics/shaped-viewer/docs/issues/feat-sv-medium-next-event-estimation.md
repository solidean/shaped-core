---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
---

# Next-event estimation never happens inside a medium

A scattering event turns and continues, picking up light only when it eventually exits.
So a lit interior converges far more slowly than a lit surface does.
It is the same gap as the shadow ray that cannot pass through glass, and equidistant sampling toward the light is the usual answer.
