---
created: 2026-10-17
parent: epic-sr-slug-open-items
refs:
  - symbol: sr::slug_routine
  - file: libs/graphics/shaped-rendering/shaders/slug_quads.sgl
---

# `slug_routine`'s pipelines refuse a scope opened with another shader's target

The routine's pipelines carry `slug_quads.sgl`'s target set.
So a scope opened with another shader's generated target refuses them.
