---
created: 2026-10-17
parent: epic-sv-denoising-integration
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/render_plan.cc
  - symbol: sv::build_render_plan
---

# The specular and split denoiser guides are declared whatever the device supports

**The specular and split guides are declared for `automatic` whatever the device supports**, because `build_render_plan` is a pure function with no context to ask.
That is five textures per denoising layer on a machine no vendor member can run, which is the price of the declaration being made before the choice is.
Only the declaration, though: the tracer writes the split lobes only when the member that resolves on this device reads them.
