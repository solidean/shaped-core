---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-graphics/docs/differential-testing.md
  - file: libs/graphics/shaped-graphics/tests/differential/differential.cc
---

# The differential corpus across machines, beyond buffers, and against the interpreter

The differential tests hold every route and backend of one machine to each other ([differential-testing](../differential-testing.md)).
What they do not do yet, in the order it pays off:

- **References across machines.**
  Since 2026-11-13 every corpus file has a committed reference, refreshed by `SC_DIFFERENTIAL_REFRESH=1` and reviewed in its diff.
  They were all written on one Arc B580; what is left is a run on an NVIDIA and an AMD GPU, and on metal, and an issue for whatever they disagree on.
- **More setups.**
  `texels` and `raster` exist since 2026-11-13; per-texel `relative` turned out to be enough for both at 16 by 16.
  A trace over a fixed scene is the next, then render targets of other formats and a depth test.
  Each is one C++ setup and one SGL module, and every corpus file naming it is free after that.
- **The interpreter as a third opinion.**
  For compute work SGL's interpreter gives the answer the language means, deterministic and with no device.
  Two routes agreeing with each other and with the interpreter is a stronger statement than two routes agreeing.
  Which corpus files the interpreter can run is the first thing to measure.

webgpu and metal take part already, where the binary has them: each is one more route of its own.
