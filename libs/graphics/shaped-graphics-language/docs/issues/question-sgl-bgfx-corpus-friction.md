---
created: 2026-10-30
---

# What translating the bgfx corpus found awkward in SGL

Translating 104 bgfx shaders into SGL for shader-rosetta compiled 89 of them, but the translators reported friction that is no compile gap.
Each is a decision about the language rather than a missing feature, and each came up in several shaders.
Every one below was reviewed and accepted with the fix named.

- **No scalar broadcast.** `max(v, 0.0)` and `clamp(v, 0.0, 1.0)` on a vector have no overload; every call spells `float3(0.0)`.
  Fix: scalar-bound overloads of `min`, `max` and `clamp`, never an implicit scalar-to-vector promotion.
  Making them generic later is `debt-sgl-prelude-scalar-broadcast-generic`.
- **`float3` and `vec3` meet badly.** `cross`, `reflect` and `refract` take `vec3` alone, while `mat3(…)` takes `float3` columns.
  Fix: a `mat3` of `vec3` columns, and the guide translates directions as `vec3`; `cross` stays on directions.
  Whether normals want a type of their own is `question-sgl-bivec3-for-normals`.
- **Swizzles are `xyzw` alone.** `.rgb` and `.r` are refused, though color code reads better with them.
  Fix: a second alphabet a `@swizzle` struct names in its annotation, `@swizzle(rgba)`, on the component-wise vectors only, so a `pos3` has no red.
  This reverses CHK-384's "`rgba` is not a second alphabet", and the rationale changes with it.
- **`return if c => a else b` does not parse**, since `=>` is looser than the keyword form; it needs `return (if c => a else b)`.
  Fix: `return`, `yield` and `break` take an if-expression as their value, put back together in the AST as a lambda's `=>` already is.
- **No `pi`** in the prelude: every file that needs it declares its own.
  Fix: a `@shadowable const pi` in `core.sgl`, so a file's own `pi` still compiles.
- **A parameter named like a builtin hides it**: a parameter `pow` makes a later `pow(…)` `unsupported-yet: a call of a local value`.
  Fix: a diagnostic that names the shadowing, not a change to shadowing.

A long expression does break across lines already, by GRP-26 and GRP-40, so the 200-character lines some translations keep are the translator's, not the language's.

The corpus's guide, shader-rosetta's `docs/translating-to-sgl.md`, lists the workarounds each one takes today.
