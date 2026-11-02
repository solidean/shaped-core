---
created: 2026-10-17
parent: epic-sr-slug-open-items
refs:
  - symbol: sr::slug_routine
  - file: libs/graphics/shaped-rendering/shaders/modules/slug.sgl
  - file: libs/graphics/shaped-rendering/shaders/slug_quads.sgl
---

# Benchmark Slug's runtime fill rule against a nonzero-only shader

The runtime fill rule and weight boost were chosen over a pipeline per variant, on the condition that they cost nothing measurable.
The benchmark against a nonzero-only shader that settles it is not written yet.
