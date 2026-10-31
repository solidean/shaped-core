---
created: 2026-10-30
---

# What the second bgfx translation wave found awkward in SGL

Translating 105 more bgfx shaders into SGL for shader-rosetta found friction that is no compile gap, each worth a decision.
The second review settled most of it: `as-precedence` with its quick fix (CHK-411), unary `+` (CHK-410) and `filled` of the innermost element (CHK-289).
So did a function's own `const` (CHK-413), `keyword-as-name` (FORM-43) and `vec3 as float3` (CHK-412).

- **No bare block statement and no empty `else`**: settled, `scope:` is the block (AST-156), and an `else` holding only a comment was allowed already.
