# Intel D3D12: a view-instanced pipeline restored from its cached blob in another process crashes the driver

**Status:** open, not yet filed upstream; worked around in shaped-graphics, which neither stores nor restores such a blob.
**Affects:** Intel Arc B580, driver 32.0.101.9150, Windows 11, the system D3D12 runtime; AMD's driver and WARP are not affected.
**Found by:** `ssc::dxc + dx12 - ctx.cached dedups raster pipelines and keys every baked state` segfaulting on every run after the first, since 2026-11-20.

## What happens

A graphics pipeline is created through a pipeline state stream holding a `VIEW_INSTANCING` subobject, and `GetCachedBlob` returns its blob.
A second process creates the same pipeline, from the same DXIL and the same root signature, with that blob as its `CACHED_PSO` subobject.
`CreatePipelineState` then takes an access violation inside `igd12um64xe2.dll`, and the process dies.

Neither a refusal nor a removed device: the call never returns.
The D3D12 contract for a blob that does not fit is `D3D12_ERROR_DRIVER_VERSION_MISMATCH` or `E_INVALIDARG`, which an application handles by building fresh.
So no application can defend itself, short of never handing the driver such a blob.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library; `shader.hlsl` is a constant quad, compiled by the Windows SDK's dxc.
Each row saves a blob in one process and restores it in a second, three times:

```
adapter                     configuration                                         restores
--------------------------  ----------------------------------------------------  ----------------
Arc B580, 32.0.101.9150     2 views                                               3 of 3 crashed
Arc B580, 32.0.101.9150     1 view, through the view-instancing subobject         3 of 3 crashed
Arc B580, 32.0.101.9150     4 views                                               3 of 3 crashed
Arc B580, 32.0.101.9150     2 views, the debug layer on                           3 of 3 crashed
Arc B580, 32.0.101.9150     no view-instancing subobject                          3 of 3 restored
Arc B580, 32.0.101.9150     2 views, restored in the process that saved           3 of 3 restored
Arc B580, 32.0.101.9150     as above, the view locations moved and overwritten    3 of 3 restored
Radeon iGPU, 32.0.21042.62  every row above                                       3 of 3 restored
WARP 10.0.26100.9818        every row above                                       3 of 3 restored
```

The blob is the driver's own, handed back unchanged to the same driver for the same description, which is the use the API defines.
The debug layer reports nothing before the crash.

## What the bug needs

- **The view-instancing subobject.** One view stated through it crashes alike, and the same stream without it restores.
- **A second process.** The process that saved the blob restores from it, so the driver presumably keeps state the blob leans on.
- **Not the address of the view locations.** Moved to another array, the first one overwritten, the restore in the saving process still succeeds.
- **Not the shaders.** A constant quad is enough; shaped-core's test pipeline, which reads a vertex attribute and binds a depth-stencil format, crashes alike.

## Reproducing it

```bash
uv run run.py
```

It builds `repro.cc` with `clang-cl` and compiles `shader.hlsl` with the newest Windows SDK's dxc.
Then it tries every row on each Intel, AMD or NVIDIA adapter it finds, and on WARP.
It exits 1 when a restore crashes, and 2 when a run stops before its restore.

## What shaped-core does about it

`dx12_raster_pipeline::skips_cached_blob` is set for a pipeline of more than one view, as it is for a mesh-shaded one: such a pipeline neither restores from a blob nor hands one out.
So the persistent pipeline tier builds it fresh every run, on every vendor.
The comment on `skips_cached_blob` in [dx12_raster_pipeline.hh](../../../libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_raster_pipeline.hh) names this directory.

A single-view pipeline goes through `CreateGraphicsPipelineState` with no view-instancing subobject, which restores correctly, so it keeps its blob.

## Re-checking after a driver update

Run `uv run run.py`.
If no row crashes on Intel, remove the view count from `skips_cached_blob` and this directory with it.
`ssc::dxc + dx12 - ctx.cached dedups raster pipelines and keys every baked state` is the test that crashed, on its second run against a warm store.
