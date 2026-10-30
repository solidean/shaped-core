---
created: 2026-10-30
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register.hh
corpus:
  - bgfx/shaders/06-bump/fs_bump
  - bgfx/shaders/06-bump/vs_bump
  - bgfx/shaders/06-bump/vs_bump_instanced
  - bgfx/shaders/33-pom/vs_pom
  - bgfx/shaders/49-hextile/fs_hextile
closed: 2026-10-30
resolution: fixed
---

# A `transpose` for `mat4` and `mat3`

The prelude has no `transpose` (CHK-397), and it is the gap that blocks the most of the shader-rosetta corpus after constant-block arrays: 5 of the first 104 bgfx translations.

- **What needs it is bgfx's `mul(v, m)`**, a row vector times the matrix, which SGL spells `transpose(m) * v`.
  bgfx writes it to take a direction into tangent space, `mul(lightDir, tbn)`, and for the camera position, `mul(vec4(0, 0, 0, 1), u_view)`.
- **Every target has it natively**: HLSL and MSL `transpose`, WGSL `transpose`.
- `feat-sgl-matrix-zoo-and-quat` holds the rest of the matrix gaps; this one is split out because it is one builtin family and the corpus needs it most.

`corpus:` names the shaders in shader-rosetta (`uv run dev.py companions init shader-rosetta`), under `sources/`; `tools/sgl-coverage.py` re-measures them.
