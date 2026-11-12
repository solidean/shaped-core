# Intel D3D12: a strict `(1 / x) * x` comes back 1 where `x` is zero

**Status:** open, not yet filed upstream; the one check that trips it is skipped on Intel's dx12 adapters, with no workaround.
**Affects:** Intel Arc B580, driver 32.0.101.9150, Windows 11; WARP is not affected.
**Found by:** the float-specials probe of shaped-graphics-test, which SGL compiles with `-Gis` at `@float(.specials)`.

## What happens

A compute shader reads `0.0` and `+infinity` from a buffer, so no compiler sees a constant.
It writes three products, each a NaN by IEEE 754: `0 * inf`, `inf * 0`, and `(1 / 0) * 0`.
The first two come back NaN on the Arc B580 too.
The third comes back `0x3f800000`, which is 1.0: the driver's compiler cancels the reciprocal against the multiply.

That is a fast-math simplification, and DXC's default DXIL licenses it, since every float operation is marked `fast`.
With `-Gis` the DXIL carries no `fast` flag, and the driver folds it all the same:

```
%9 = fdiv float 1.000000e+00, %4
%10 = fmul float %4, %9
```

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library; the shader is HLSL compiled by the Windows SDK's dxc.
`uv run run.py` runs it on the high-performance adapter and on WARP, with `-Gis` and without:

```
DXIL            adapter     0 * inf     inf * 0     (1 / 0) * 0
--------------  ----------  ----------  ----------  -----------
strict (-Gis)   Arc B580    0xffc00000  0xffc00000  0x3f800000
strict (-Gis)   WARP        0xffc00000  0xffc00000  0xffc00000
fast            Arc B580    0xffc00000  0xffc00000  0x3f800000
fast            WARP        0xffc00000  0xffc00000  0xffc00000
```

The DXIL is the same for both adapters, and strict, so a result that differs from WARP's on it is the driver's.
The fast row is no bug, since `fast` licenses the fold; it is there to show that `-Gis` changes nothing on this driver.

## Reproducing it

    uv run run.py

It exits 1 when the hardware adapter returns a product that is no NaN from strict DXIL while WARP returns NaNs, and 0 when it does not.

## What we do about it in the tree

Nothing works around it: SGL's `-Gis` is the right request, and no spelling of the shader we know of avoids the fold.
`compute-float-specials-test.cc` skips its `(1 / 0) * 0` check on Intel's hardware adapters, and checks it everywhere else.
The same test sees the fold on Intel's vulkan driver too, from SPIR-V that keeps NaNs and contracts nothing; this reproduction is D3D12's alone.
[bug-sg-intel-dx12-zero-times-inf](../../../libs/graphics/shaped-graphics/docs/issues/bug-sg-intel-dx12-zero-times-inf.md) tracks it.

## What to re-check after a driver update

Run `uv run run.py`; once it exits 0, remove the skip in `compute-float-specials-test.cc` and close the issue.
