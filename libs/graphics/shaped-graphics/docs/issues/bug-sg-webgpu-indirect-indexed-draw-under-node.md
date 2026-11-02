---
created: 2026-11-02
refs:
  - test: "sg - indirect indexed draws read every field from the GPU, and an instance count of 0 draws nothing"
  - file: libs/graphics/shaped-graphics/tests/pipeline/draw-params-test.cc
---

# Indirect indexed draws draw nothing on webgpu under node

What is wrong or wanted, and what is already known.

On `emscripten-webgpu-relwithdebinfo` under node (Dawn), Windows, 2026-11-02, every pixel of the test stays at the clear value: no indirect indexed draw lands.
It fails every run, and it fails on the pushed tree too, so it predates the debug window that was being built when it was found.
The direct indexed draws of the test before it pass, which points at the indirect path: its first-instance field, or the indirect buffer's own handling.
