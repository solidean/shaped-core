---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/specialization.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
closed: 2026-11-05
resolution: fixed
---

# The 2-state opacity-micromap ray flag, and opacity_micromap as a feature

sg builds opacity micromaps on vulkan, and a trace honors one with no change to the shader (sg `tests/pipeline/ray-query-test.cc`).
Two ordinary pieces are missing on the SGL side: the ray flag that forces micromaps to 2-state, a builtin, and `opacity_micromap` among the features a `require` names.
dx12 reaches micromaps through Shader Model 6.9's `RAY_FLAG_FORCE_OMM_2_STATE`; metal and webgpu have none, so the feature is refused there statically.
This closes the "no issue yet" in the comparison matrix's B8 cell.

## Resolution

Done in 5a008f936: `ray_flags.force_omm_2_state`, which DXR and vulkan both give as 0x400, on dx12 and vulkan alike (EMIT-184).
`opacity_micromap` stayed a feature no program names: an HLSL compile for a device that has micromaps states it for every ray query.
sr's micromap bake test traces a baked quad forced to 2 states on both.
