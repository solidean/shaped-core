# Intel D3D12: view instancing misbehaves unless the pixel stage reads SV_ViewID

**Status:** open, not yet filed upstream; worked around in the shaders that draw by view.
**Affects:** Intel Arc B580, driver 32.0.101.9150 (view instancing tier 2), Windows 11; WARP is not affected.
**Found by:** `sxr::controller_routine` reporting success on dx12 and writing no pixel, while vulkan drew it correctly on the same GPU.

## What happens

One draw of two views, into a two-layer target, with a pixel stage that does not read `SV_ViewID`.
If the vertex stage moves the position by view, the draw writes nothing, and every layer keeps its clear value.
No call fails, and the D3D12 debug layer reports no error.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library; the shaders are HLSL compiled by the Windows SDK's dxc.
`uv run run.py` gave this table, the same on each of its runs:

```
configuration                                      hardware   WARP
position by view, pixel stage ignores the view     nothing    drew
position by view, pixel stage reads the view       drew       drew
fixed position, pixel stage ignores the view       hung       drew
fixed position, pixel stage reads the view         drew       drew
neither stage declares the view                    hung       drew
```

`hung` means the process had not finished after 15 seconds, and `run.py` killed it; where it stopped is not known.
An earlier version of the shaders, with no varying between the stages, failed in `CreatePipelineState` instead, with `DXGI_ERROR_DRIVER_INTERNAL_ERROR`.

## What the bug needs

- **The pixel stage not reading `SV_ViewID`.** It has to really use it: the workaround below compares it, so the compiler cannot fold it away.
- **More than one view.** One view through the same stages is what every other pipeline does.
- **Not a matrix, constants or depth.** The repro has none of them.
- **Not a vertex buffer.** `sg`'s test fails with one, and the repro without one.
- **Not the driver's shader cache.** A shader changed by a meaningless constant fails on its first compile.
  That separates this from [intel-d3d12-cached-pso-draws-nothing](../intel-d3d12-cached-pso-draws-nothing/readme.md).

## What we do about it

A pixel stage of a view-instanced pipeline reads `@view_index`.
`libs/graphics/shaped-xr/shaders/controller.sgl` and `examples/graphics/xr-cube/shaders/xr_cube.sgl` both do.
`sg - one draw of two views moves geometry with only the vertex stage reading which` is the case itself, and skips on Intel's dx12.

Nothing enforces this for a new shader, so one that forgets draws nothing on this hardware.
SGL could add the read itself for a view-instanced pipeline on `hlsl-dx12`, which would retire the rule.

## When the vendor ships an update

Run `uv run run.py`: it exits 1 while the bug reproduces and 0 once the adapters agree.
Then drop the skip in `pixel-semantics-test.cc`, and the workaround in both shaders.
