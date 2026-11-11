---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/compute
---

# Work graphs on dx12

A work-graph state object, its backing memory, and `DispatchGraph` on a command list, behind a feature dx12 grants at SM 6.8.
The description comes from SGL's host code generation, as a ray-tracing pipeline's does.

Microsoft has reportedly frozen work graphs at SM 6.9.
So this is worth doing only if [feat-sgl-work-graphs](../../../shaped-graphics-language/docs/issues/feat-sgl-work-graphs.md) decides native support pays.
Whether the system runtime has them or only the Agility SDK is unchecked.
