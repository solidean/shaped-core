---
created: 2026-10-17
parent: epic-sgl-feature-grants
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/raytracing_pipeline.cc
---

# Features across a hot reload outside a `pipeline`

A declared SGL pipeline freezes its features, so a reload needing another one keeps what it had.
A compute shader or a stage acquired on its own has no frozen part: its reload compiles, and its pipeline is then refused by the feature's name.
