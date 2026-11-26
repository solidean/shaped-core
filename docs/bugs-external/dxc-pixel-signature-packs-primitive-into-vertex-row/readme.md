# DXC packs a pixel stage's per-primitive input into a vertex input's row, which AMD's D3D12 driver draws wrong

**Status:** reproduced standalone, not yet filed upstream with either DXC or AMD.
Worked around in SGL's own DXIL (EMIT-286); DXC's route still meets it on AMD's dx12.
**Affects:** DXC 1.9.2602.17 (the Windows SDK's) and 1.9.2602.24 (the repo's), `-pack-prefix-stable` and `-pack-optimized` alike.
Drawn wrong by the AMD Radeon iGPU (RDNA 2), D3D12 driver 32.0.21042.62, Windows 11; the Arc B580 and WARP draw it right.
**Found by:** `sg - a mesh stage's per-primitive values reach a pixel stage of SGL's binary and of DXC's alike`, then part of the mixed-route mesh test.

## What happens

A mesh stage writes a quad as two triangles, each with a per-primitive `weight`, and each vertex an `id`.
The pixel stage reads both, so every pixel of the 8x8 target is covered and (1, 1) reads the second triangle's weight, (1.5, 3).

```
id is            pixel input signature (dxc)        AMD iGPU                     Arc B580  WARP
---------------  ---------------------------------  ---------------------------  --------  -----
nointerpolation  ID row 1 x, WEIGHT row 1 yz        wrong: 10 of 64 covered,     right     right
                                                    (1, 1) never written
linear           ID row 1 x, WEIGHT row 2 xy        right                        right     right
```

DXC packs a pixel stage's inputs first fit, and a per-primitive input interpolates as a constant, so it fills the free columns of any `nointerpolation` vertex row.
In the fixture that first found it, the vertex link held a `nointerpolation int`, and AMD covered no pixel at all.
Any one per-primitive attribute beside one constant vertex attribute is enough; a linear vertex attribute is never shared, so the second row is the control.

## Why it is DXC's packing, and AMD's reading of it

The D3D12 mesh shader spec, section "Signature Linkage", says:

> Vertex and primitive attributes will not be packed into the same row, even when the interpolation mode matches.
> The elements will be packed with vertex elements first, with primitive elements packed starting on the row immediately following the last row used by the vertex elements.

DXC's signature breaks that, and both DXC versions validate and sign it.
The same spec links a mesh pipeline's stages by semantic, and Intel and WARP draw the shared row right, so a driver could read it.
AMD's evidently reads the primitive rows from the first row past the vertex ones, as the spec promises it may.
`repro.cc` is D3D12 and the C++ standard library, with an empty root signature, and the two rows differ in one interpolation modifier.

## Reproducing it

    uv run run.py

It compiles both pairs with the Windows SDK's dxc, prints the pixel stage's input rows, and draws each on every hardware adapter named AMD, Intel or NVIDIA, and on WARP.
It exits 1 when DXC shares the row and the AMD adapter draws it wrong while drawing the linear pair right, 0 when it draws both right, and 2 when the run is inconclusive.

## What we do about it in the tree

EMIT-286: SGL's own DXIL starts a pixel stage's per-primitive inputs on the row after its vertex inputs, as the spec says, so SGL's pixel stage draws on AMD beside either route's mesh stage.
HLSL has no way to place a pixel input, so DXC's route keeps the shared row.
sg's `known_issues.cc` entry `dxc-pixel-signature-packs-primitive-into-vertex-row` names AMD's dx12 for it; the test above holds SGL's pixel stage there and skips DXC's.

## What to re-check after an update

Run `uv run run.py` after a DXC or an AMD driver update.
Once it exits 0, or the listing shows `WEIGHT` on a row of its own, the known issue may retire: `uv run dev.py test --known-issues` runs the skipped pairings.
