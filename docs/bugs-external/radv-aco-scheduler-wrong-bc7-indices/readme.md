# RADV: ACO's scheduler gives a spilling BC7 encoder wrong indices in five blocks

**Status:** open, reproduced standalone, not yet filed upstream, its one test skipped on that driver; measured on 2026-11-25.
**Affects:** RADV in Mesa 26.2.0-devel (git-5c65d4fc31), an AMD Steam Machine (NAVI33, RDNA 3), SteamOS; DXC 1.9.0.5399's SPIR-V.
**Found by:** `shaped-rendering-test`'s `sr - BC7 compression reproduces every two-subset partition and holds a gradient` on `sr vulkan / vulkan`, every run, max error 121.
**Tracked by:** [bug-sr-bc7-two-subset-radv-aco-scheduler](../../../libs/graphics/shaped-rendering/docs/issues/bug-sr-bc7-two-subset-radv-aco-scheduler.md).

## What happens

The shader is SGL's BC7 encoder: one thread per 4 x 4 block, each block fitted as two subsets over its own partition, in mode 1 and then mode 3, keeping the closer.
The image is 64 blocks, block p holding two four-level ramps that only partition p fits exactly, so every block's mode 3 encode decodes to within 2 of its source.
As RADV compiles it, five blocks come out wrong: 19, 27, 35, 43 and 51.
Their endpoints are right, and their 2-bit indices are not, in both subsets: subset 0 reads 0, 2 and 3 where its ramp is 0, 1, 2, 1, …, 3.
In all five, subset 1's endpoints were swapped, so the index flip after the fit ran; many blocks that take that flip come out right.
With `ACO_DEBUG=nosched` every block is right.

## The trigger

The module spills: ACO's dump of it holds 4336 VGPR spill and reload operations, and no private array in memory, as compiled and with the scheduler off alike.
`ACO_DEBUG=validateir,validatera` reports nothing, and the five blocks still come out wrong under it.

| configuration | wrong blocks |
|---|---|
| as compiled | 19, 27, 35, 43, 51 |
| `ACO_DEBUG=nosched` | none |
| `ACO_DEBUG=novn` | 19, 27, 35, 43, 51 |
| `ACO_DEBUG=noopt` | 3, 11, 19, 27, 35, 43, 51 |
| DXC at `-O3`, `-O1`, `-O0` or `-Od` | 19, 27, 35, 43, 51 at each |
| SGL's own SPIR-V of the same encoder, in shaped-core | none |

`shader.hlsl` is SGL's `hlsl-vulkan` text of the encoder, cut down by hand and still failing.

| cut | bug |
|---|---|
| the texture read replaced by a buffer of rgba8 words | stays |
| mode 6, and the search over 64 partitions for the two closest | stays |
| two candidate partitions down to one, the block's own | stays |
| the push constants replaced by literals | stays; with only the bounds check literal, the wrong blocks are 3, 11, 27, 35, 43, 51 |
| mode 1, so mode 3 runs alone | **gone** |
| the least-squares refit of either subset | **gone** |
| the block's opacity fixed at compile time, which lets DXC fold the mode choice | **gone** |

So the two modes have to run in one loop, each subset refitted, with the mode chosen at run time.
A hand-written port of the same algorithm, in float3 and with its own helper functions, does not fail; the shape the generated code has is part of the trigger.
That the wrong blocks move when unrelated code changes points at code generation rather than at the data.

## Why it is not our bug

`repro.cc` is the Vulkan loader and the C++ standard library, and decodes the blocks itself.
`spirv-val` accepts DXC's module, and the result does not change with DXC's optimization level, `-Od` included.
On the same device and driver the module comes out right with one compiler pass switched off, and its validators find nothing.
SGL's own SPIR-V of the same encoder passes on that device.

Not ruled out: undefined behavior in the module that only the scheduler's order exposes.
No second compiler for this GPU was available to settle it: this RADV build has no LLVM backend, and lavapipe is not installed.

## Reproducing it

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

It compiles `shader.hlsl` with the SDK's `dxc` as shaped-core does, checks it with `spirv-val`, builds `repro.cc` with clang against the SDK, and runs it twice.
Both runs set `RADV_DEBUG=nocache`; the second also sets `ACO_DEBUG=nosched`.
It exits 1 when blocks are wrong only with the scheduler on, and 0 when every block is right.
On the machine above it prints:

```
as compiled:
    device: AMD Steam Machine (RADV NAVI33), vendor 1002, radv Mesa 26.2.0-devel (git-5c65d4fc31)
    bad=19,27,35,43,51 other=none
ACO's scheduler off:
    device: AMD Steam Machine (RADV NAVI33), vendor 1002, radv Mesa 26.2.0-devel (git-5c65d4fc31)
    bad=none other=none
REPRODUCED: blocks 19,27,35,43,51 decode wrong as compiled, and every block is right with ACO's scheduler off
```

## In the tree

Nothing works around it.
`bc-compress-test.cc` skips the partition test by sg's known issue of this name: a vulkan context on the DXC route, on `radv` whose `driverInfo` starts `Mesa 26.2.0-devel`.
A Mesa update past that version runs the test again, and so does `uv run dev.py test --known-issues`.

## Re-checking

On a Mesa update, run `run.py`; an exit of 0 retires this entry, its issue and the skip.
An exit of 1 on a new version wants that version added to the entry's key in `known_issues.cc`.
A report to Mesa wants `RADV_DEBUG=shaders` output of both arms beside the module, and would settle the undefined-behavior question.
