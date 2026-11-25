---
created: 2026-11-25
refs:
  - issue: feat-sgl-interpreter-bounds-float-departures
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/textures.cc
  - file: libs/graphics/shaped-graphics/tests/differential/interpreted.cc
---

# A bounding run bounds texture reads and interpolated varyings

The interpreter does run texture reads: loads, filtered samples, levels from a quad's derivatives, gathers (EVAL-120 to EVAL-126).
A bounding run takes them exactly, though: a ranged coordinate is read at its exact value, and the texel comes back with no range.
The texels setup covers it with relative 0.005 across APIs, since a filtered sample's weights carry as few as 8 fractional bits on some hardware.

## Texture reads

- **A filtered sample's range**: the hull of the filter at the coordinate's ends, widened by the weights' precision.
  The APIs state a weight's subtexel precision, 8 bits on D3D and at least 4 on Vulkan, so the stated bound exists.
  A measured one per device follows the shape of `feat-sgl-interpreter-bounds-builtin-precision`.
- **A level** picked from a ranged derivative is a range of levels, so a trilinear sample's range takes both.
- **A load at a ranged integer coordinate** is unbounded, as an index is today.

## Interpolated varyings

- The raster setup's interpreter half gives each pixel its varyings exactly at the pixel's center, as its vertex stage writes them.
  A GPU interpolates them in its own precision, perspective-correct, from the vertex outputs, and the varyings are where the error comes in.
- A varying's range: the exact barycentric interpolation, widened by what the APIs allow an interpolator.
  What each API states for that is not looked up yet; D3D's functional spec is the place to start.
- The setup would hand the interpreter ranged varyings rather than exact ones, and everything else follows from the ranges already there.
