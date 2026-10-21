---
created: 2026-10-17
parent: epic-sv-drawings-and-decals
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/drawing/drawing_manager.cc
  - symbol: sr::build_slug_blas
---

# Shapes are not traced as geometry of their own yet

Module `slug` cuts quads down to their shapes with `slug.decide` (libs/graphics/shaped-rendering/docs/slug.md), which is what a label casting a letter-shaped shadow needs.
`sr::build_slug_blas` builds those quads as a BLAS.
Decals paint surfaces that already exist.
What is missing for shapes as geometry is the scene side that places their quads and the rows that trace them.
