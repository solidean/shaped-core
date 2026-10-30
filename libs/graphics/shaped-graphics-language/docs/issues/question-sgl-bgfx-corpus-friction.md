---
created: 2026-10-30
---

# What translating the bgfx corpus found awkward in SGL

Translating 104 bgfx shaders into SGL for shader-rosetta compiled 89 of them, but the translators reported friction that is no compile gap.
Each is a decision about the language rather than a missing feature, and each came up in several shaders.

- **No scalar broadcast.** `max(v, 0.0)` and `clamp(v, 0.0, 1.0)` on a vector have no overload; every call spells `float3(0.0)`.
  GLSL, HLSL and WGSL all take the scalar.
- **`float3` and `vec3` meet badly.** `cross`, `reflect` and `refract` take `vec3` alone, while `mat3(…)` takes `float3` columns.
  A shader mixing them converts both ways, `vec3(..x)` and `float3(..x)`.
- **Swizzles are `xyzw` alone.** `.rgb` and `.r` are refused, though color code reads better with them.
- **`return if c => a else b` does not parse**, since `=>` is looser than the keyword form; it needs `return (if c => a else b)`.
- **A bracket does not continue across plain lines**, and an if-expression split inside parentheses reads as a tuple, so some expressions stay on one 200-character line.
- **No `pi`** in the prelude: every file that needs it declares its own.
- **A parameter named like a builtin hides it**: a parameter `pow` makes a later `pow(…)` `unsupported-yet: a call of a local value`.

The corpus's guide, shader-rosetta's `docs/translating-to-sgl.md`, lists the workarounds each one takes today.
