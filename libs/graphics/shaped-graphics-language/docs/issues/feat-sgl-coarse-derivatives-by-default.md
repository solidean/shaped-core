---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_math.cc
---

# `ddx` and `ddy` are coarse on metal too

EVAL-144 makes `ddx`, `ddy` and `fwidth` coarse, and `ddx_coarse`, `ddx_fine`, `ddy_coarse` and `ddy_fine` exist explicitly.
Every target writes that but metal (EMIT-126, EMIT-238): HLSL's `ddx_coarse`, WGSL's `dpdxCoarse`, SPIR-V's `OpDPdxCoarse` and DXIL's `derivCoarseX`.
MSL has no coarse spelling, so metal still writes `dfdx` and `dfdy` for both, which are fine on Apple GPUs.

What is left is the coarse default on metal, from the quad's own lanes, and the probe below that pins it.

## Measured on metal

On 2026-11-09 on an M4, MSL's `dfdx` and `dfdy` are fine.
A pixel stage writing `ddx(x * y)` and `ddy(x * y)` at pixel centers over a 4x4 target read each pixel's own row's `y` and own column's `x`.
So the two rows of a quad differ, 0.5 and 1.5, where a coarse derivative would give both the same.
`ddx_fine` and `ddy_fine` are therefore `dfdx` and `dfdy` on metal, and the coarse default wants the quad's own lanes: `quad_shuffle` of one row's or column's pair, a hypothesis not yet tried.
That probe, written as a test beside `sg - a discarded pixel still takes part in its quad's derivatives`, is what would pin the coarse default on every backend.

## Log

2026-11-13: under fast math, which an entry point at `.fast` compiles with on metal since EMIT-262, Apple's backend writes `dfdx` and `dfdy` coarse.
The AIR is the same in both modes but for the float flags, so the choice is the backend's; the differential corpus's `pixel_derivatives` sees it as about 0.023 on its bent value.
So metal's default derivatives are coarse wherever no `@float` is stated, which is EVAL-144's meaning, and fine at any stricter level.
`pixel_derivatives` holds them loosely across machines until every target writes coarse.
A `ddx_fine` on metal needs a guarantee above `.fast`, or a probe that pins what fast math does to `dfdx` at all.
