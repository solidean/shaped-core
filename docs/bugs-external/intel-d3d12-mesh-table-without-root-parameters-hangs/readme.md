# Intel D3D12: a mesh stage that reads a constant table hangs the device under a root signature of no parameter

**Status:** reproduced standalone, not yet filed upstream; sg's dx12 backend gives an empty root signature one unread constant.
**Affects:** Intel Arc B580, D3D12 driver 32.0.101.9150, Windows 11, on the system runtime.
The AMD Radeon iGPU beside it and WARP run every row.
**Found by:** reducing the mesh stages of the per-primitive repro, whose first corners came from a table.
[dxc-pixel-signature-packs-primitive-into-vertex-row](../dxc-pixel-signature-packs-primitive-into-vertex-row/readme.md) is that repro.

## What happens

`shader.hlsl`'s mesh stage alone is the pipeline: one vertex, no triangle, no pixel stage, no target and no resource.
With the table, the vertex's x is `xs[WaveGetLaneIndex() & 1]` of a `float xs[2]`, which DXC states as an `internal constant` global.

```
mesh stage                root signature                      Arc B580                       AMD iGPU  WARP
------------------------  ----------------------------------  -----------------------------  --------  ----
the table                 no parameter                        device removed, 0x887a0006     kept      kept
the table                 one 32-bit constant, never set      kept                           kept      kept
a select of both values   no parameter                        kept                           kept      kept
```

`0x887a0006` is `DXGI_ERROR_DEVICE_HUNG`.
The constant is in a register space nothing declares, so it changes nothing the shader can see, and a root UAV does as well.
A pixel stage reading such a table under an empty root signature runs; a compute stage was tried only under a root signature of parameters, where it runs too.
Where the table's read reaches does not matter: a table of triangle indices in a loop hangs the device the same way.

## Why it is not our bug

`repro.cc` is D3D12 and the C++ standard library: one root signature, one two-subobject pipeline stream, one `DispatchMesh`.
Both root signatures are valid, the container is DXC's own, validated and signed, and a root parameter no shader reads is meant to be inert.

## What is not settled

sg's frozen-shader build, which compiles shader diagnostics out, also builds a mesh pipeline under a root signature of no parameter.
It draws SGL's table-reading mesh stage there on the Arc, from a container byte for byte the one that hangs here.
So the hang has one more ingredient than this repro holds and sg's command stream lacks, or the reverse.
Not it, each tried in this host: the input-assembler root flag, a bound descriptor heap, the debug layer, DRED, feature level 11_0.
Nor sg's whole pipeline stream with its empty amplification stage, an input-assembler topology, the bind order, another draw ahead, a pause before the draw, debug info.

## Reproducing it

    uv run run.py

It runs the three rows on each hardware adapter named Intel, AMD or NVIDIA, and on WARP.
It exits 1 when the Intel adapter loses the device on the first row and keeps it on the other two, 0 when it keeps it on all three, and 2 when the run is inconclusive.

## What we do about it in the tree

`dx12_pipeline_layout.cc` gives a root signature that would hold no parameter one 32-bit constant no shader declares, which nothing sets.
sg's layouts have one only where shader diagnostics are compiled out, since the debug window is a root parameter otherwise.
The mixed-route test's `tabled` mesh and task stages read tables on every route.

## What to re-check after a driver update

Run `uv run run.py`; once it exits 0, the constant may go.
