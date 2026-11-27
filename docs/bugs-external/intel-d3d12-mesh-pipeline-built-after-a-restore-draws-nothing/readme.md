# Intel D3D12: a mesh pipeline built fresh after one was restored from another process's blob draws nothing

**Status:** reproduced standalone, not yet filed upstream; sg's dx12 backend restores no mesh-shaded blob on an Intel GPU.
**Affects:** Intel Arc B580, D3D12 driver 32.0.101.9150, Windows 11, on the system runtime.
The AMD Radeon iGPU beside it and WARP draw every row.
**Found by:** shaped-graphics' mesh tests, once dx12 mesh pipelines restored from the persistent tier: a mesh pipeline drew nothing after other tests had restored theirs.
shaped-graphics' issue bug-sg-intel-dx12-mesh-draws-nothing-after-blob-restores is the history.

## What happens

`shaders.hlsl` is one full-screen green triangle: a mesh stage of three vertices, and a pixel stage passing its color on.
One process builds the pipeline and saves its `GetCachedBlob`.
A second process builds it from that blob, then builds the same stream again without one, and draws with each:

```
second process                         Arc B580                          AMD iGPU  WARP
-------------------------------------  --------------------------------  --------  -----
mesh: restored, then built fresh       restored draws, fresh draws none  draws     draws
mesh: built fresh, then restored       draws                             draws     draws
vertex: restored, then built fresh     draws                             draws     draws
```

A blob saved earlier in the same process does not do it: save, restore, then build fresh draws every time.
Every pipeline the process builds fresh after the restore draws nothing, not only the first.
No call fails, and the device is not removed.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library, and the shaders are DXC's, validated and signed.
The two pipelines are one stream, one with the blob and one without, and the one without is what goes wrong.

## Reproducing it

    uv run run.py

It runs every row on each hardware adapter named Intel, AMD or NVIDIA, and on WARP.
It exits 1 when the Intel adapter draws nothing for the first row and draws the other two, 0 when it draws every row, and 2 when the run is inconclusive.

## What we do about it in the tree

`dx12_bind.cc` creates every raster pipeline with `mesh_blobs` false on an Intel GPU, so a mesh-shaded pipeline there neither restores a blob nor hands one out.
A vertex pipeline still does, which the third row says is safe.
Not ruled out: that the same restores raise the rate of shaped-viewer-test's heap corruption, bug-sv-test-heap-corruption-with-real-cache, which rose while they were allowed.

## What to re-check after a driver update

Run `uv run run.py`; once it exits 0, `mesh_blobs` may go.
