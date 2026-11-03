---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_math.cc
---

# `ddx` and `ddy` are coarse on every target, with `_coarse` and `_fine` spelled out

`ddx` and `ddy` must not be implementation-defined, which they are today on at least one target.
EMIT-126 writes them as HLSL's `ddx` and `ddy`, MSL's `dfdx` and `dfdy` and WGSL's `dpdx` and `dpdy`, and each of those leaves coarse or fine to the implementation.

- **`ddx` and `ddy` are coarse on every target**: `ddx_coarse` in HLSL and `dpdxCoarse` in WGSL; MSL has no coarse spelling, which is the part to find out.
- **`ddx_coarse`, `ddx_fine`, `ddy_coarse` and `ddy_fine` exist explicitly**, for a shader that needs the per-pixel difference.
- **That the default is coarse is documented prominently**, in the spec and wherever derivatives are introduced, since a port from HLSL or GLSL may assume fine.

Coarse is the default because it may be faster, above all where a texture is sampled.
Whether the interpreter (EVAL: a derivative in a run of one invocation is 0) needs to tell the two apart is part of the work.
