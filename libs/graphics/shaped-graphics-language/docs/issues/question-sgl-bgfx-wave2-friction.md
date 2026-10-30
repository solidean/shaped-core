---
created: 2026-10-30
---

# What the second bgfx translation wave found awkward in SGL

Translating 105 more bgfx shaders into SGL for shader-rosetta found friction that is no compile gap, each worth a decision.

- **`as` binds looser than every arithmetic operator**, and the error says nothing about it: `x as float - y as float` is `unsupported-yet: this expression as a type`.
  Three translators hit it; a diagnostic that names the precedence, `parenthesize: (x as float) - …`, would have saved each a round.
- **Unary `+` parses (OP-9) and has no operator**: `float2(+half.x, …)` is `no-matching-overload: operator +(float)`.
  It should probably be a prelude operator of every number, a no-op.
- **`filled` of a 2D array takes a row**: `float[4, 4].filled(0.0)` is a type mismatch, and the zeroed grid reads `float[4, 4].filled(float[4].filled(0.0))`.
  Filling every scalar of a nested array with one value is the common case.
- **No bare block statement and no empty `else`**: a `{ … }` scope of the original, or an `else { /* debug */ }`, has no spelling, so the translations flatten or drop them.
- **A `const` stands at file scope alone**: a function's `const sphere_solid_angle = 4.0 * pi` moves out of the function.
- **`var` is a keyword**, so a local the original names `var` is renamed.
- **`hpos4` and `vec3` back to a plain vector** read `float4(..h)` and `float3(..v)`, which a port needs often; a `.float4` or `as float4` would be shorter.
