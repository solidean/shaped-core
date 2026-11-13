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
  `scene` exists since 2026-11-13: inline traces over a fixed TLAS, ids exact by value and hits within relative 0.0001 across APIs.
  `targets` exists since 2026-11-13: five formats at once behind a depth test, decoded to floats, the integer one exact.
  `blended` blends into them over a clear, sRGB in linear space included; a multisampled resolve is next.
  Its interpreter half binds the software pool a `software_trace` build writes, so 12 of the 13 entry points now run on the interpreter.
  Each is one C++ setup and one SGL module, and every corpus file naming it is free after that.
- **The interpreter as a third opinion.**
  Since 2026-11-13 a fourth test holds every configuration to SGL's interpreter: 11 of the 12 entry points run there, and all agree.
  The pixel stages run as quads, each lane its own pixel (EVAL-148), so derivatives and implicit-level samples are checked too.
  Left out is `workgroup_reduction`, whose workgroup memory spans invocations the interpreter runs one at a time.
  A workgroup would need the interpreter to run a workgroup's invocations together, up to each barrier, which is a change to the interpreter itself.

webgpu and metal take part already, where the binary has them: each is one more route of its own.
