---
created: 2026-10-30
corpus:
  - bgfx/shaders/39-assao/cs_assao_generate_q0
  - bgfx/shaders/39-assao/cs_assao_generate_q1
  - bgfx/shaders/39-assao/cs_assao_generate_q2
  - bgfx/shaders/39-assao/cs_assao_generate_q3
  - bgfx/shaders/39-assao/cs_assao_generate_q3base
closed: 2026-10-31
resolution: fixed
---

# A loop that must not be unrolled, HLSL's [loop]

SGL has `@unroll` (CHK-389) and nothing for its opposite, which HLSL spells `[loop]` and bgfx `LOOP`.
ASSAO's sample loop carries it, since an unrolled body of that size costs registers and compile time for nothing.

- **Shapes to weigh**: `@unroll(false)`, or a `@loop` of its own; and whether it is a promise to the target or a hint.
- **Target reach**: HLSL has `[loop]`, MSL and WGSL have no portable spelling, so on two targets it would be a no-op.
- The five translations mark the place with `// gap:` and loop without it.
