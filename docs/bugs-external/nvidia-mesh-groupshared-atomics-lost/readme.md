# NVIDIA: a mesh stage loses groupshared `InterlockedOr` results when a late return reads a flag set before the barrier

**Status:** open, reproduced standalone, not yet filed upstream, its one test skipped on NVIDIA in shaped-rendering; measured on 2026-11-09.
**Affects:** NVIDIA D3D12 driver 621.01, GeForce RTX 5070 Ti, Windows 11; DXC 1.8.2502.11 from the Windows SDK 10.0.26100.
**Found by:** the shaped-rendering test `sr - a mesh stage's read of workgroup memory no invocation stored is reported where it would have run (LEGAL-69)`.
It fails there on dx12 and vulkan alike, by DXC's route and SGL's native one; the standalone reproduction is D3D12 alone.

## What happens

A mesh shader of four invocations clears a groupshared word, and after a group barrier, every invocation but #2 `InterlockedOr`s its own bit into it.
After a second group barrier, every invocation reads the word back with `InterlockedOr(marks, 0, seen)` and writes it out.
Every invocation must read `0xb`.
On the RTX every invocation reads `0x1`: the bit of invocation 0 survives, and the bits of 1 and 3 are lost.

The trigger is a flag, `aborted`, that is never true.
It is set in a guard that is never taken (`k >= 4` in a group of four), gates the `InterlockedOr`, and gates a `return` after `SetMeshOutputCounts`:

```hlsl
bool aborted = false;
if (k != 2)
{
    if (k >= 4)
        aborted = true;
    if (!aborted)
        InterlockedOr(marks, 1u << k);
}
GroupMemoryBarrierWithGroupSync();
// read marks, write it out
SetMeshOutputCounts(4, 2);
if (aborted)
    return;
```

SGL emits exactly this shape: a bounds check on an array index sets the flag, and the entry point ends the invocation on it after its outputs are counted.

## The trigger

Each row is one container, run as one workgroup in a fresh process; "kept" is `b b b b`.

| shader | RTX 5070 Ti |
|---|---|
| the HLSL SGL generates for LEGAL-69, with the read written out | **lost**, `1 1 1 1` |
| … without its debug-print blocks | **lost** |
| … without its index guards (so without the flag) | kept |
| … without the late `return` | kept |
| `shader.hlsl`, `LATE_RETURN=1` (above) | **lost**, `1 1 1 1` |
| … with a plain device store or nothing in the guard instead of a device atomic | **lost** |
| … compiled at `-Od` | kept |
| … returning on `k >= 4` instead of on the flag (`LATE_RETURN=0`) | kept |
| … with the flag computed before the branch, `aborted = k >= 4` | kept |
| … with the `k != 2` branch folded into the mark's condition | kept |
| … with invocation 2 marking too (no `k != 2`) | kept, `f f f f` |
| … as a compute stage | kept |

So the bug needs all of these together:

- the flag set inside the divergent `k != 2` branch, so it reaches the barrier as a phi;
- that phi live across the barrier, read by the late `return`;
- the mark's `InterlockedOr` inside the same branch;
- the mesh stage, since the compute twin keeps every mark;
- DXC's optimized DXIL, since `-Od` keeps every mark.

The optimized DXIL is short and right.
The phi `[ %9, %7 ], [ %9, %10 ], [ 0, %5 ]` meets the barrier, the `atomicrmw or` on `@marks` sits in block `%10` before it, and the branch on the phi follows `SetMeshOutputCounts`.

A guess, not measured: on NVIDIA hardware, mesh-stage shared memory lives beside the outputs, with no native atomics.
Mesa's [NVK mesh shading notes](https://docs.mesa3d.org/drivers/nvk/mesh_shading_notes.html) say so.
The driver then has to emulate the atomics, and a divergent exit after the barrier seems to throw off that emulation.

## Why it is not our bug

The shader is twenty lines of HLSL with nothing of ours, compiled by the stock DXC.
The DXC validator accepts it, and its disassembly is a direct reading of the source.
The four controls differ from the failing shader by one optimization flag, one spelling of the same always-false condition, or the stage.
Each keeps every mark on the same GPU and driver.

The in-box WARP 10.0.26200 is no reference: it removes the device on every mesh pipeline, the passing controls included.
No AMD or Intel GPU was at hand for this run.

## Reproducing it

    uv run run.py [--dxc <dxc.exe>] [--vendor <hex PCI id>]

It builds `repro.cc` with clang-cl and compiles `shader.hlsl` with the newest Windows SDK's dxc unless `--dxc` names one.
Each container runs in a process of its own, on the first NVIDIA adapter unless `--vendor` names another.
It exits 1 when the failing shader loses marks and every control keeps them, 0 when nothing is lost, and 2 when the run is inconclusive.
On the machine above:

```
mesh, return on the flag         marks lost [1 1 1 1]         NVIDIA GeForce RTX 5070 Ti
mesh, return on the flag, -Od    every mark [b b b b]         NVIDIA GeForce RTX 5070 Ti
mesh, return on k >= 4           every mark [b b b b]         NVIDIA GeForce RTX 5070 Ti
compute                          every mark [b b b b]         NVIDIA GeForce RTX 5070 Ti
REPRODUCED: the mesh stage loses groupshared InterlockedOr results when a late return reads the flag
```

## What we do about it

The LEGAL-69 mesh test in [debug-output-test.cc](../../../libs/graphics/shaped-rendering/tests/debug-output-test.cc) skips on an NVIDIA hardware adapter, dx12 and vulkan alike, naming this directory.
SGL still emits the flag, so a robustness-profile mesh shader on NVIDIA can still report a read of workgroup memory that another invocation stored.
Ending an aborted mesh invocation without a flag live across a barrier would avoid the bug; that is not done.

## What to re-check

On a driver update, run `run.py` on an NVIDIA GPU.
Exit 0 retires this entry and the skip with it.
