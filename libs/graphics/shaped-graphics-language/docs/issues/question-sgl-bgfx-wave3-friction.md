---
created: 2026-10-30
---

# What the third bgfx translation wave found awkward in SGL

Translating the last 159 bgfx shaders for shader-rosetta found friction that is no compile gap.

- **A library cannot call back without naming its caller's bindings**: S2H's `s2h_print_character` calls a shader's own callback.
  So it must list `{scatter, params}`, the shader's own names, or it is `binding-not-listed`.
  A shared library of helpers that take a callback needs the effect list to be generic, or inferred.
- **A leading-dot case is refused as a call argument** (CHK-152's Open point): `gather(p, s, component = .y)` must spell `texel_component.y`.
- **A `mut` receiver has no dot call**: `ui.print(…)` has nowhere to put the `mut`, so S2H's translation repeats `s2h_print_txt(mut ui, …)` hundreds of times.
- **No descending or float range**: `for (int i = 7; i >= 0; --i)` and `for (float w = 0; w < 8; w += 1)` become an int range and a derived local.
- **A vector constructor converts no component**: `float3(a * 4u + b, …)` of uints is written `float3(uint3(…))`.
- **A literal expression does not convert to `half`** as a bare literal does: `half2(half(2.0 / 5.0))`.
- **`type` is a keyword**, and the parser's error is `expected-expression` rather than one that names the keyword.
- **The VS Code extension lags the compiler**: an installed extension reports `.rgb` on a plain vector as `unknown-member`, which the compiler accepts; `uv run dev.py install sgl-vscode` refreshes it.
