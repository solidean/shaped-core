---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/examples/openpbr-spheres.cc
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
---

# A dense subsurface interior is still darker than its `subsurface_color`

Far less so than it was.
`openpbr-spheres`' subsurface row is the case: black with sparse red speckle before roulette and after it, at 64 frames and at 2048.

Index-matching that row's interface was what located it.
The row went from black to a dark red, which said the interface rather than the walk was taking it.
Total internal reflection is now fixed, which is what the row's present colour comes from.

What is left after that:

- **The per-channel survival.**
  `subsurface_color` (0.88, 0.52, 0.42) inverts to albedos of about (0.995, 0.922, 0.865), against mean free paths of (0.010, 0.0035, 0.0020) at the sweep's low end.
  So red outlives green and blue by an order of magnitude.
- **The missing next-event estimation inside a medium**, its own issue.
