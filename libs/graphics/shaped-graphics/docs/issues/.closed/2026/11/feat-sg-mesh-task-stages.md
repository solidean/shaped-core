---
created: 2026-10-17
parent: epic-sg-raster-gaps
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/raster-pipeline.md
closed: 2026-11-05
resolution: fixed
---

# Mesh and task stages

Geometry and tessellation stages are in on dx12 and vulkan; mesh and task (amplification) stages are not.

## Resolution

Done on 2026-11-05 on dx12 and vulkan: `sg::feature::mesh_shader`, `raster_pipeline_description::mesh_shader` and `task_shader`, and `draw_mesh_tasks(x, y, z)`.
dx12 builds a mesh pipeline as a pipeline-state stream with AS and MS subobjects; vulkan enables VK_EXT_mesh_shader wherever the device has `meshShader` and `taskShader`.
`tests/pipeline/mesh-test.cc` draws a mesh-shaded triangle, and a task stage launching two mesh workgroups with one payload, on both.
Left for later: an indirect mesh draw, a mesh pipeline of several views, and metal's object and mesh stages.
