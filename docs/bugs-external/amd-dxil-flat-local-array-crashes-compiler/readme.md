# AMD: a local array held as one flat array of floats crashes the driver's compiler

**Status:** open, reproduced standalone, not yet filed upstream, worked around in SGL's DXIL writer; measured on 2026-11-27.
**Affects:** AMD's D3D12 shader compiler `amdxc64.dll` 8.18.10.0514, the one Radeon GPU Analyzer 2.15 bundles, for RDNA 3 and RDNA 4.
CI met it on a Radeon RX 9060 XT (RDNA 4, gfx1200), creating a compute pipeline from SGL's native DXIL.
8.18.10.0500, AMD's driver 32.0.21042.62 on the RDNA 2 iGPU of a Ryzen 9 9950X3D2, compiles it.
**Found by:** shaped-graphics' native-dxil sweep on CI's AMD runner: the process ended in `CreateComputePipelineState`.

## What happens

The compiler reads past a buffer's end while compiling a compute pipeline, and the process ends with an access violation.
It never returns an error.
On the reporter's run under cdb, CI's driver and RGA's bundled compiler fault at one instruction, `mov edx, dword ptr [r10+rcx*4+10h]`, with `rcx` near 0x14000.

The shape is a local array of eight 16-float elements, held as one `[128 x float]` alloca:

1. zero-filled where it is declared,
2. then rewritten whole, every element and field, inside a branch,
3. then one field of each element written in a loop whose count is known at run time alone.

SGL's DXIL writer held a local `Vertex[8]` of a 16-float struct exactly so; DXC splits the same array into sixteen `[8 x float]`, one per field and lane.

## The trigger

Each row is `flat.hlsl` compiled by DXC 1.9.2602, with the switch named, through RGA 2.15's compiler for gfx1200.

| module | gfx1200 |
|---|---|
| `flat.hlsl` | **crashed** |
| without step 1, `-DNO_FILL` | compiled |
| without step 2, `-DNO_REWRITE` | compiled |
| without step 3, `-DNO_FIELD` | compiled |
| step 2 unconditional, `-DUNCONDITIONAL` | compiled |
| step 2 rewriting 7 of the 8 elements, `-DREWRITTEN=7` | compiled |
| 4 elements, `-DELEMENTS=4` | compiled |
| 16 elements, `-DELEMENTS=16` | compiled |
| `split.hlsl`, the same program over a struct array DXC splits | compiled |

So all three steps are needed, the rewrite under a branch and over every element, and the array's length matters.
The reporter also found that step 2 written out with constant indices still crashes, so loop unrolling is no fix.

Across chips and compiler versions:

| compiler | chip | `flat.hlsl` | `split.hlsl` |
|---|---|---|---|
| RGA 2.15's, 8.18.10.0514 | gfx1200, gfx1201 (RDNA 4) | **crashed** | compiled |
| RGA 2.15's, 8.18.10.0514 | gfx1100, gfx1103 (RDNA 3) | **crashed** | compiled |
| RGA 2.15's, 8.18.10.0514 | gfx1030, gfx1036 (RDNA 2) | no dx12 target in RGA | no dx12 target in RGA |
| the driver's, 8.18.10.0500 | RDNA 2 iGPU, on the hardware | compiled | compiled |

RGA reports the crash as `failed to extract pipeline binary`; only the gfx1200 one was seen under a debugger.

## Why it is not our bug

Both modules are DXC's own compile of plain HLSL, and nothing of ours touches them.
DXC's validator accepts both, and the flat array is ordinary DXIL: DXC writes it for any `float[128]` an HLSL source declares.
An out-of-bounds read inside the driver's compiler is the driver's to fix.

## Reproducing it

    uv run run.py [--dxc <dxc.exe>] [--rga <rga.exe>] [--chip <gfxNNNN>] [--hardware]

It compiles `flat.hlsl` and `split.hlsl` with the newest Windows SDK's dxc unless `--dxc` names one.
By default each goes through RGA's offline compiler for gfx1200, which needs no AMD GPU: [RGA's releases](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer/releases) have it.
`rga.exe` is looked for on PATH and under `C:\tools`, unless `--rga` names it.
`--hardware` builds `repro.cc` with clang-cl instead, and creates each pipeline on the first AMD adapter, a process each.
It exits 1 when the flat module crashes and the split one compiles, 0 when both compile, and 2 when the run is inconclusive.
On the machine above:

```
split (DXC's sixteen [8 x float])    compiled
flat (one [128 x float])             crashed
REPRODUCED: RGA's compiler for gfx1200 crashes on the flat array and compiles the split one
```

`--hardware` on its RDNA 2 iGPU prints `not reproduced: the adapter's driver compiles both`.

## What we do about it

SGL's DXIL writer splits a variable, workgroup memory or table that holds an array of structs, vectors or matrices as DXC does: a column per field and lane (EMIT-293).
[writer_memory.cc](../../../libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer_memory.cc) lays it out in `lay_out_memory`, which names this directory.
`slib - SGL's DXIL splits a local array of structs a column per field and lane, as DXC does (EMIT-293)` holds the layout there.
`sg - an SGL array of structs keeps each field, in a variable, in workgroup memory and in a table` runs it on every driver.
The split is DXC's own layout and costs nothing measurable, so it stays when AMD fixes the compiler.

## What to re-check

On an RGA or driver update, run `run.py`, and `run.py --hardware` on an AMD GPU.
Exit 0 on gfx1200 retires this entry; the split stays either way.
The RDNA 2 iGPU's driver predates the crashing compiler, so its next update is worth a `--hardware` run.
