---
created: 2026-10-30
---

# What the third bgfx translation wave found awkward in SGL

Translating the last 159 bgfx shaders for shader-rosetta found friction that is no compile gap.
The second review settled most of it: a callback brings its own bindings (CHK-319), a leading-dot case is an argument (CHK-152), a dot call binds a `mut` receiver (CHK-416), ranges walk in reverse and in steps (CHK-415), and `type` as a name is `keyword-as-name` (FORM-43).

- **A vector constructor converts no component**: `float3(a * 4u + b, …)` of uints is written `float3(uint3(…))`.
  [question-sgl-plain-vectors-as-layout](question-sgl-plain-vectors-as-layout.md) is where the plain vectors' permissiveness is being talked through.
- **The VS Code extension lags the compiler**: an installed extension reports `.rgb` on a plain vector as `unknown-member`, which the compiler accepts; `uv run dev.py install sgl-vscode` refreshes it.
