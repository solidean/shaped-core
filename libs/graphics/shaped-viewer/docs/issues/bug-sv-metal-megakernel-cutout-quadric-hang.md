---
created: 2026-10-28
refs:
  - test: "sv::pathtrace_routine - every material spelling traces a lit, finite image"
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_raytracing_shader_table.cc
  - issue: feat-sv-path-tracer-on-metal
---

# On metal, the megakernel hangs the GPU on a scene of cutout triangles and quadrics

`sv::pathtrace_routine` on metal loses the device (`MTL4CommandQueueErrorTimeout`) when one scene holds a non-opaque triangle mesh and a quadric batch.
`cross-backend-trace-test.cc`'s `cutout` case is that scene, and it skips on metal until this is fixed.
The wavefront integrator, the viewer's default, traces the same scene correctly.

What is narrowed down so far:

- **The any-hit bodies do not matter.** Generated `sv_any_hit` and `sv_shadow_any_hit` reduced to `return hit_decision.accept` still hang.
  So does a scene whose triangles have any-hits at all, which a non-opaque BLAS asks for.
- **Neither half hangs alone.** The same case without the quadrics traces, and the `quadrics` case traces.
- **sg's own mix does not hang.** `sg - an SGL ray-tracing pipeline traces non-opaque triangles and procedural boxes in one scene` passes on metal.
  Its groups are the module's own, and its shadow rays never reach a box.

What sv adds over that test, any of which is the next thing to bisect:

- both hit groups are compiled by the host at run time and linked as binary functions;
- shadow rays traced from a closest hit reach the procedural boxes, through `hit_1`'s table;
- the procedural group's closest hit traces shadow rays of its own.

Shader validation (`MTL_SHADER_VALIDATION=1`) reports nothing before the timeout.
