---
created: 2026-10-30
---

# What translating the bgfx corpus found awkward in SGL

Translating 104 bgfx shaders into SGL for shader-rosetta found friction that is no compile gap.
Each is a decision about the language rather than a missing feature, each came up in several shaders, and every one was reviewed and accepted with the fix named.
Scalar bounds for `min`, `max` and `clamp` (CHK-406), `pi` (CHK-405), `return if` (AST-155) and a diagnostic for a local hiding a function have landed.
What remains:

- **`float3` and `vec3` meet badly.** `cross`, `reflect` and `refract` take `vec3` alone, while `mat3(…)` takes `float3` columns.
  Fix: a `mat3` of `vec3` columns, and the guide translates directions as `vec3`; `cross` stays on directions.
  Whether normals want a type of their own is `question-sgl-bivec3-for-normals`.
- **Swizzles are `xyzw` alone.** `.rgb` and `.r` are refused, though color code reads better with them.
  Fix: a second alphabet a `@swizzle` struct names in its annotation, `@swizzle(rgba)`, on the component-wise vectors only, so a `pos3` has no red.
  This reverses CHK-384's "`rgba` is not a second alphabet", and the rationale changes with it.

A long expression does break across lines already, by GRP-26 and GRP-40, so the 200-character lines some translations keep are the translator's, not the language's.

The corpus's guide, shader-rosetta's `docs/translating-to-sgl.md`, lists the workarounds each one takes today.
