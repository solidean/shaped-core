---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/specialization.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
---

# The 2-state opacity-micromap ray flag, and opacity_micromap as a feature

sg builds opacity micromaps on vulkan, and a trace honors one with no change to the shader (sg `tests/pipeline/ray-query-test.cc`).
Two ordinary pieces are missing on the SGL side: the ray flag that forces micromaps to 2-state, a builtin, and `opacity_micromap` among the features a `require` names.
dx12 reaches micromaps through Shader Model 6.9's `RAY_FLAG_FORCE_OMM_2_STATE`; metal and webgpu have none, so the feature is refused there statically.
This closes the "no issue yet" in the comparison matrix's B8 cell.
