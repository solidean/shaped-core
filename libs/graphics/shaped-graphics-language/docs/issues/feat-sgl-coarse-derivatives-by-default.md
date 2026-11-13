---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_math.cc
---

# `ddx` and `ddy` are coarse on every target, with `_coarse` and `_fine` spelled out

`ddx` and `ddy` must not be implementation-defined, which they are today on at least one target.
EMIT-126 writes them as HLSL's `ddx` and `ddy`, MSL's `dfdx` and `dfdy` and WGSL's `dpdx` and `dpdy`, and each of those leaves coarse or fine to the implementation.

- **`ddx` and `ddy` are coarse on every target**: `ddx_coarse` in HLSL and `dpdxCoarse` in WGSL; MSL has no coarse spelling, so metal needs an emulation.
- **`ddx_coarse`, `ddx_fine`, `ddy_coarse` and `ddy_fine` exist explicitly**, for a shader that needs the per-pixel difference.
- **That the default is coarse is documented prominently**, in the spec and wherever derivatives are introduced, since a port from HLSL or GLSL may assume fine.

Coarse is the default because it may be faster, above all where a texture is sampled.
Whether the interpreter (EVAL: a derivative in a run of one invocation is 0) needs to tell the two apart is part of the work.

## Measured on metal

On 2026-11-09 on an M4, MSL's `dfdx` and `dfdy` are fine.
A pixel stage writing `ddx(x * y)` and `ddy(x * y)` at pixel centers over a 4x4 target read each pixel's own row's `y` and own column's `x`.
So the two rows of a quad differ, 0.5 and 1.5, where a coarse derivative would give both the same.
`ddx_fine` and `ddy_fine` are therefore `dfdx` and `dfdy` on metal, and the coarse default wants the quad's own lanes: `quad_shuffle` of one row's or column's pair, a hypothesis not yet tried.
That probe, written as a test beside `sg - a discarded pixel still takes part in its quad's derivatives`, is what would pin the coarse default on every backend.
