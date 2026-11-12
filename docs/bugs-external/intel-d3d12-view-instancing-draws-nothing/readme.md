# Intel D3D12: a view-instanced draw writes nothing unless the right output varies

**Status:** open, not yet filed upstream; worked around in the shaders that draw by view.
**Affects:** Intel Arc B580, driver 32.0.101.9150, view instancing tier 2, Windows 11; WARP is not affected.
**Found by:** `sxr::controller_routine` reporting success on dx12 and writing no pixel, while vulkan drew it correctly on the same GPU.

## What happens

One draw of two views, into a two-layer target.
Depending on which outputs vary, the draw writes no texel in either layer, or `CreatePipelineState` removes the device.
It is all or nothing: both views land or neither does, and no pixel lands in the wrong layer.
No call reports a misuse, and the D3D12 debug layer has no complaint except the driver's own `DXGI_ERROR_DRIVER_INTERNAL_ERROR`.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library; the shaders are HLSL compiled by the Windows SDK's dxc.
`uv run run.py` gives this, the same on every run, with the debug layer on and silent:

```
configuration                                WARP      Arc B580, 3 runs
nothing varies                               drew      nothing
nothing varies, 1 view                       drew      drew
a constant varying                           drew      nothing
a varying by vertex                          drew      drew
a varying by vertex, pixel ignores it        drew      drew
a varying by view                            drew      drew
pixel reads the view                         drew      drew
position by view                             drew      nothing
position by view, 1 view                     drew      drew
position by view, a varying by vertex        drew      CreatePipelineState (0x887a0005)
position by view, a varying by view          drew      drew
position by view, pixel declares the view    drew      nothing
position by view, pixel reads the view       drew      drew
```

All of it is valid D3D12, and WARP draws every row.
SGL's own output for `sxr::controller_routine`, run through an earlier version of `repro.cc`, failed and worked the same way, so `sg` plays no part.

## What the bug needs

Read as rules, for more than one view:

- **The position does not depend on the view:** some other output must vary, by vertex or by view, or the pixel stage must read `SV_ViewID`.
- **The position depends on the view:** some other output must depend on the view, or the pixel stage must read `SV_ViewID`.
- **Reading means reading.** Declaring `SV_ViewID` in the pixel stage without using it changes nothing.
- **An output the pixel stage ignores still counts.** Only the vertex stage has to write it.
- **One view is safe.** Every row drew with one.

dxc's `Outputs dependent on ViewId` in `-Fc` output separates the rows of the second rule, which suggests the driver plans the draw from it.
That is a guess, and nothing here says why these are the rules.

## What it does not need

Each was changed in an earlier version of this repro, on the failing rows, and none made one draw:

- **Target size:** 4 x 2 and 640 x 720.
- **Depth:** with and without a depth target.
- **Shader model:** 6.1 and 6.8, and `SV_StartVertexLocation` at 6.8.
- **Vertex count:** 3 to 14 vertices.
- **Root constants:** none, 4 and 36, read by either stage or both.
- **An interpolated varying** rather than a flat one.
- **A vertex buffer:** `sg`'s test fails with one, and this repro without one.
- **The driver's shader cache:** a shader changed by a meaningless constant fails on its first compile.
  That separates this from [intel-d3d12-cached-pso-draws-nothing](../intel-d3d12-cached-pso-draws-nothing/readme.md).

## What is not known

- **Which symptom a failing pipeline gets.** With other root signatures and shader models, some rows that write nothing removed the device instead.
  Through `sg`, the row that removes the device here wrote nothing.
- **Any other Intel GPU or driver.** Only this one was tried, and no second vendor's hardware.
- **A pipeline with no pixel stage,** such as a stereo depth prepass, which cannot use the workaround below.

## What we do about it

`sg::feature::pixel_view_read_workaround` is set on Intel's dx12 hardware, and SGL's `keep_view(view)` reads the view only there, through `const if has`.
A pixel stage of several views takes `@view_index` and multiplies its color by it.
`libs/graphics/shaped-xr/shaders/controller.sgl` and `examples/graphics/xr-cube/shaders/xr_cube.sgl` both do.
SGL warns of a `pipeline` declaration that does not (CHK-515, `pixel-stage-ignores-view`).
`sg - one draw of two views moves geometry with only the vertex stage reading which` is the case itself, and skips on Intel's dx12.

A hand-built pipeline gets no such warning, and neither does a pixel stage that takes the view and leaves it unread.
`known_issues.py` has no probe for it, because its probes may not start a subprocess and only a device can name the adapter.

## When the vendor ships an update

`uv run run.py` exits 1 while the bug reproduces, 0 once the adapters agree, and 2 if the debug layer objects to the run.
A debug-layer complaint means the repro itself is wrong, and its results say nothing about the driver.
Once it exits 0, stop setting the feature for that driver and drop the skip in `pixel-semantics-test.cc`.
