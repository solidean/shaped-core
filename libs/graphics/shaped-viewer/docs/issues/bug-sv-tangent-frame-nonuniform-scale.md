---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_frame.sgl
---

# A non-uniform instance scale shears the tangent frame

The hit rotates the authored frame by the object-to-world matrix and renormalizes.
That is exact for a rigid or uniformly scaled placement and wrong for anything else.
The tangent wants the matrix itself while the normal wants the inverse transpose, so the two cannot both come out of one rotation of one frame.

The GEOMETRIC normal is not affected: both hit shaders take it through `h.normal_to_world`, which is the inverse transpose.
`quadric-gallery` places a batch under a non-uniform scale on purpose.
What is left is the authored frame alone, and it needs the frame decomposed rather than rotated.
