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
What SGL needs, each piece with no counterpart yet:

* `check::stage` values and attributes `@task(x, y, z)` and `@mesh(x, y, z, vertices = N, primitives = M)`, the counts HLSL's `out vertices` and `out indices` arrays need at compile time.
* A mesh stage's output, a value of the stage alone like a geometry stage's `triangle_stream[T]`: counts set once in uniform control flow, then vertices and triangles by index.
* A task stage's `dispatch_mesh(x, y, z, payload)`, and the mesh stage's `@payload` parameter of the same struct.
* The `mesh_shader` feature among SGL's, refused on WGSL by EMIT-109 and on MSL until sg's metal backend wires the stages up.
* slib: the stage mapping in `sgl_compiler.cc`, and `mesh` and `task` among a `pipeline` block's stages in the prelude's mirror, `sgl_host_code.py` and `pipeline.cc`.
