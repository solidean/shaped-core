---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# Mesh and task stages

SGL has vertex, pixel, compute, geometry and tessellation stages, and no mesh or task stage.
dx12, vulkan and metal have both; WebGPU has neither.

sg's side is [feat-sg-mesh-task-stages](../../../shaped-graphics/docs/issues/.closed/2026/11/feat-sg-mesh-task-stages.md).
SGL's side is the two stages, their outputs, and a feature that refuses them on webgpu.
A compute polyfill is the incubator's idea for the geometry stages there ([stage-polyfills.md](../spec/incubator/stage-polyfills.md)), and could serve these too.

## sg's side is done (2026-11-05)

`sg::feature::mesh_shader`, `raster_pipeline_description::mesh_shader` and `task_shader`, and `draw_mesh_tasks` run on dx12 and vulkan, tested from HLSL in sg's `tests/pipeline/mesh-test.cc`.
`sg::shader_stage::task` and `mesh` exist, DXC compiles them as `as_` and `ms_`, and an HLSL package names them.
## The mesh stage is done (2026-11-05)

`@mesh(x, y, z, vertices = N, triangles = M)` and `mut mesh_output[T]`, with `set_counts`, `vertex` and `triangle` (CHK-480 to CHK-482).
A `pipeline` names it as `mesh = f` (CHK-483), HLSL writes it on dx12 and vulkan (EMIT-205), and WGSL and MSL refuse it (EMIT-109, EMIT-206).
slib maps the stage, and a generated pipeline acquires and builds it; sg's `tests/pipeline/mesh-test.cc` draws two SGL mesh pipelines on dx12 and vulkan.
Left:

* The task stage: `@task(x, y, z)`, `dispatch_mesh(x, y, z, payload)` as the last statement of its body, and the mesh stage's `@payload` parameter of the same struct.
  HLSL wants the payload in `groupshared` memory and `DispatchMesh` called once, in uniform control flow.
* Per-primitive attributes: a second struct of `mesh_output`, written by `o.primitive(i, p)`, which the pixel stage reads beside the vertex's.
* Line and point topologies, which the attribute would name.
* metal, once sg's metal backend builds a mesh pipeline.
