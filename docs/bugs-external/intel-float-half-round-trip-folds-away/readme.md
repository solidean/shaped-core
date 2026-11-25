# Intel: a float's round trip through a half comes back unrounded

**Status:** open, not yet filed upstream; shaped-core's differential test logs it through a `known` line, with no workaround.
**Affects:** Intel Arc B580, driver 32.0.101.9150, Windows 11, on D3D12 and on vulkan, each in one spelling.
WARP is not affected.
**Found by:** `half_math.sgl` of shaped-graphics' differential corpus, whose `(x as half) as float` came back as `x` on dx12.

## What happens

A compute shader reads 256 floats from a buffer, most of which no half holds, so no compiler sees a constant.
It converts each to a half and back, in two spellings: a pair of casts, and HLSL's `f32tof16` and `f16tof32`.
A half holds 11 significant bits, so whichever way the conversion rounds, the result has its low 13 bits clear.

On the Arc B580 one spelling per API comes back as the input, the conversion folded away:

```
API     adapter   DXIL / SPIR-V   casts                     f32tof16 / f16tof32
------  --------  --------------  ------------------------  ------------------------
D3D12   Arc B580  strict (-Gis)   no half, 256 of 256       halves, toward zero
D3D12   Arc B580  fast            no half, 256 of 256       halves, toward zero
D3D12   WARP      strict (-Gis)   halves, toward zero       halves, toward zero
D3D12   WARP      fast            halves, toward zero       halves, toward zero
vulkan  Arc B580  strict (-Gis)   halves, to nearest        no half, 256 of 256
vulkan  Arc B580  fast            halves, to nearest        no half, 256 of 256
```

The first input, `0x3efe7280`, comes back `0x3efe7280` where it fails, where a half holds `0x3efe8000` to nearest or `0x3efe6000` toward zero.

## Why it is not our bug

`repro.cc` and `vulkan/repro.cc` are D3D12, DXGI, Vulkan and the C++ standard library; the shader is HLSL compiled by the SDKs' own dxc.
- **D3D12:** the DXIL is the same for both adapters, and WARP returns halves from it, so a result that is no half is the driver's.
  No fast-math flag licenses removing a conversion, so the fast row is the same bug, not a license.
- **Vulkan:** the SPIR-V keeps `OpFConvert` for the casts and `PackHalf2x16` and `UnpackHalf2x16` for the intrinsics, as `spirv-dis` shows.
  So the fold is the driver's, and no rounding mode makes the input a half.
- **The pinned dxc of shaped-core** (`--dxc`, `--profile cs_6_6`) gives the same table.

## What is not the bug

The repro began as `half_arithmetic.sgl`'s `a * b - a`, which looked like a fused multiply-add the driver performed under `-Gis` too.
It was not: the differential test's strict-floor compiles had lost their floor inside shaped-core's shader library, so they ran at `.fast`, where the fusion is allowed.
Measured on the corpus kernel, Intel rounds every half operation as binary16 does under `-Gis` and under HLSL's `precise`, on both APIs.
It fuses only where nothing forbids it: on vulkan without `NoContraction`, 64 of 256 elements.

## Reproducing it

    uv run run.py [--adapter <substring>] [--vulkan-sdk <dir>] [--dxc <dxc.exe>] [--profile cs_6_2]

It exits 1 when a hardware adapter returns a round trip that is no half, on D3D12 where WARP returns halves from the same DXIL, and 0 when every round trip is a half.

## What we do about it in the tree

Nothing works around it yet.
`half_math.sgl` carries `known intel dx12` lines for it, at its own level and at the strict-floor twin's.
SGL writes `(x as half) as float` as the casts on every target, so the vulkan spelling never reaches a shader of ours.
On D3D12 the intrinsics round, toward zero, which would be a workaround only where toward zero is acceptable; EVAL-96 says to nearest.
[bug-sg-diff-half-corpus-fails-on-intel](../../../libs/graphics/shaped-graphics/docs/issues/bug-sg-diff-half-corpus-fails-on-intel.md) tracks it.

## Open

- D3D's WARP converts a float to a half toward zero, through the casts and through `f32tof16` alike, where EVAL-96 rounds to nearest.
  Whether D3D specifies toward zero is not looked up; if it does, `bug-sgl-half-conversion-rounds-toward-zero-on-amd` describes D3D's rule rather than AMD's bug.

## What to re-check after a driver update

Run `uv run run.py`; once it exits 0, run the differential tests, remove the `known intel dx12` lines of `half_math.sgl` they then fail on, and close the issue.
