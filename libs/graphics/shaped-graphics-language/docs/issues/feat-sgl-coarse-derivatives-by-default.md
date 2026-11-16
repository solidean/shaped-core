---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_math.cc
---

# metal's derivatives mean EVAL-144's at every `@float` level

EVAL-144 makes `ddx`, `ddy` and `fwidth` coarse and `ddx_fine` and `ddy_fine` the per-pixel difference, on every target.
Every target writes that but metal (EMIT-126, EMIT-238): HLSL's `ddx_coarse`, WGSL's `dpdxCoarse`, SPIR-V's `OpDPdxCoarse` and DXIL's `derivCoarseX`.
MSL has neither spelling, and metal writes `dfdx` and `dfdy` for both, which Apple's backend makes coarse under fast math and fine otherwise (measured below).
So metal's `ddx` is fine above `.fast`, and its `ddx_fine` coarse at it.

A `@float` level licenses departures in float arithmetic alone (EVAL-6), and which lanes of a quad a derivative subtracts is no arithmetic, so it must not follow the math mode.
The fix is metal's emitter building both from the quad's own lanes, `quad_shuffle` of one row's or column's pair, at every level and whatever the backend picks for `dfdx`.
A probe beside `sg - a discarded pixel still takes part in its quad's derivatives`, run at `.fast` and above, would pin the meaning on every backend.
Then `pixel_derivatives` in the differential corpus no longer needs its loose bound across machines.

## Measured on metal

On 2026-11-09 on an M4, under the safe math every compile had then, MSL's `dfdx` and `dfdy` are fine.
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

2026-11-16: the `@float` level is not to change a derivative's meaning, since EVAL-6 licenses float arithmetic alone; the fix is the emitter's, whatever Apple's backend does to `dfdx`.
