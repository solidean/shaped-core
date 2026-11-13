---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-graphics/docs/differential-testing.md
  - file: libs/graphics/shaped-graphics/tests/differential/differential.cc
---

# The differential corpus across machines, beyond buffers, and against the interpreter

The differential tests hold every route and backend of one machine to each other ([differential-testing](../differential-testing.md)).
Three things they do not do yet, in the order they pay off:

- **References across machines.**
  One machine sees only its own GPUs, so an NVIDIA-only or an AMD-only divergence is invisible on a dev box with an Arc.
  An exact output can commit a hash per (file, entry point, output), which any machine checks.
  A float output cannot, since vendors round transcendentals apart; a small committed reference held to the file's `across` bound is the shape.
  A machine that disagrees should be able to write what it saw, so the question becomes "who is right" rather than "something moved".
  How a reference is refreshed, and who may refresh it, is the open design question.
- **More setups.**
  `texels` and `raster` exist since 2026-11-13; per-texel `relative` turned out to be enough for both at 16 by 16.
  A trace over a fixed scene is the next, then render targets of other formats and a depth test.
  Each is one C++ setup and one SGL module, and every corpus file naming it is free after that.
- **The interpreter as a third opinion.**
  For compute work SGL's interpreter gives the answer the language means, deterministic and with no device.
  Two routes agreeing with each other and with the interpreter is a stronger statement than two routes agreeing.
  Which corpus files the interpreter can run is the first thing to measure.

webgpu and metal take part already, where the binary has them: each is one more route of its own.
