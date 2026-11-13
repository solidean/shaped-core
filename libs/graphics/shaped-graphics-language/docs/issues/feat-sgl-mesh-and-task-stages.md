---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-06T05:08Z
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
The task stage and per-triangle values are done too (2026-11-05): `@task(x, y, z)` and `dispatch_mesh` with a payload the mesh stage takes (CHK-484 to CHK-487, EMIT-207, EMIT-208).
`mesh_output[T, F]` writes `F` per triangle, which the pixel stage takes as a second struct (CHK-488, CHK-489, EMIT-209).
sg's mesh-test.cc draws a task stage launching two mesh workgroups, and a quad of two differently tinted triangles, on dx12 and vulkan.
Left:

* ~~metal~~: landed 2026-11-09, as `[[object]]` and `[[mesh]]` functions over Metal 4's mesh render pipeline (EMIT-206).
  sg's SGL mesh tests run on metal from tests/pipeline/sgl-mesh-test.cc, which no longer waits for DXC.
* Points, which D3D12's mesh stage cannot write; vulkan alone has them.

## Lines and an indirect draw (2026-11-06)

`@mesh(…, lines = M)` writes lines through `o.line(i, l)`, and its pipeline rasterizes a line list (CHK-480, CHK-482, CHK-483, EMIT-205).
sg's `draw_mesh_tasks_indirect` reads each draw's workgroup counts from a buffer of `sg::dispatch_args`.
sg's mesh-test.cc draws two crossing lines, and an indirect draw of one record and of two, on dx12 and vulkan.
