---
created: 2026-11-06
priority: normal
tags: [xr, dx12]
refs:
  - test: shaped-xr session - a hand is drawn where its pose puts it, on dx12
  - file: libs/graphics/shaped-xr/shaders/controller.sgl
closed: 2026-11-06
resolution: fixed
---

# A view-dependent position draws nothing through sxr::controller_routine on dx12

On dx12, on an Intel Arc B580 (driver 32.0.101.9150), `sxr::controller_routine` reports `executed` and writes no pixel.
The same routine draws correctly on vulkan, on the same GPU, in both eyes, where each eye's pose puts it.
`xr-cube --dx12` selects a matrix by view the same way, so it may be affected too, and nothing has read its pixels yet.

## What it needs

Every row is the routine on dx12 over Monado, read back from a frame cleared to a known color.

| Vertex stage | Pixels written |
|---|---|
| The real transform, a matrix chosen by `view` | 0 |
| The same, with depth test and write off in the pipeline | 0 |
| The same, into textures made by hand rather than the session's | 0 |
| The transform of each eye computed, and the result chosen by `view` | 0 |
| A fixed quad shifted by `view`, with no matrix | 0 |
| The real transform, always the left eye's, `view` unused | 1024 per eye |
| A fixed quad, `view` unused | 115200 per eye |

So the position depending on `SV_ViewID` is what stops the draw here.

The matrices are right: drawing a fixed quad and writing the transformed points as color reads the same on dx12 and vulkan, for the origin and for all three axes, in both eyes.

## What it does not need

- **Not the driver's shader cache:** a shader changed by a meaningless constant fails on its first compile too, so this is not `intel-d3d12-cached-pso-draws-nothing`.
- **Not the adapter:** the context is on the same Arc as `sg`'s own dx12 tests.
- **Not misuse the debug layer sees:** with it on, it reports only clear values.

These `sg` tests all pass on that Arc and on WARP, and each adds one ingredient of the failing draw:

- `sg - one draw of two views puts the same geometry somewhere else in each`, a position shifted by view.
- The same over a depth array, and the same from `@vertex_index` with a triangle strip and no vertex buffer.

## Resolved (2026-11-06)

It is Intel's driver, not sg: a view-instanced draw there writes nothing unless the right output varies, and the external bug has the rules.
Reading `SV_ViewID` in the pixel stage satisfies them in every case tried.
`sg - one draw of two views moves geometry with only the vertex stage reading which` shows it on the Arc, with WARP drawing.
[The external bug](../../../../../../../../docs/bugs-external/intel-d3d12-view-instancing-draws-nothing/readme.md) reproduces it with no code of ours.
Both shaders that draw by view now read it in the pixel stage, and the dx12 hand test passes on the Arc.
