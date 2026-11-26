# AMD vulkan: a 64-bit atomic store to workgroup memory leaves its high word, which an atomic `or` then reads

**Status:** open, not yet filed upstream; worked around in SGL, which writes the store as an exchange (EMIT-258).
**Affects:** AMD Radeon iGPU (RDNA 2), vulkan driver 2.0.353 (AMD proprietary shader compiler 2026.04.17), Windows 11.
The Arc B580 beside it has no `shaderSharedInt64Atomics`, so it runs none of this.
**Found by:** `sg vulkan - workgroup 64-bit atomics on a device that has them (CHK-510)`, once SGL's own SPIR-V became the default.
That test picks the first vulkan device with the feature, which on this machine is the AMD iGPU.

## What happens

One workgroup of 64 threads shares a 64-bit sum.
Thread 0 zeroes it, every thread adds 2^33 plus its index with `OpAtomicIAdd`, and thread 0 reads the total back: 2^39 + 2016, high word `0x80`, low word `0x7e0`.
Barriers with `AcquireRelease | WorkgroupMemory` stand between the three steps.
Each row of `shader.spvasm` zeroes the sum and reads it its own way; every module passes `spirv-val`:

```
zeroed by              read by             scope       AMD iGPU
---------------------  ------------------  ----------  -----------------------------------------
OpAtomicStore          OpAtomicLoad        Device      right
OpAtomicStore          OpAtomicLoad        Workgroup   right
OpAtomicExchange       OpAtomicLoad        Workgroup   right
OpAtomicStore          OpAtomicOr of 0     Workgroup   wrong: high 0x2908ca9d, low 0x000007e0
OpAtomicExchange       OpAtomicOr of 0     Workgroup   right, the shape DXC writes
```

The wrong row is wrong on every run, its low word right and its high word different each time.
That reads as a store that zeroed the low word alone, leaving whatever the workgroup memory held above it, which the 64 adds then carried.
An atomic load after the same store reads right, so the load sees a value the `or` does not.

## Why it is not our bug

`repro.cc` is Vulkan and the C++ standard library, and the shader is SPIR-V assembly that the Vulkan SDK's `spirv-as` assembles and `spirv-val` accepts.
The rows differ in one instruction each, and the store and the `or` are both atomics of one `Workgroup` variable, ordered by the barriers.
So no row has a race or an undefined value, and every one has one right answer.

## What is not settled

The issue's first table, measured through SGL's whole shader on 2026-11-07, had every plain form wrong and its high word lost rather than kept.
That shader also takes a signed 64-bit `min` in a second workgroup variable.
This repro, on the driver installed on 2026-11-26, shows the store followed by an `or` alone.
Whether the driver changed between the two, or the second variable is part of the trigger, is not known.

## Reproducing it

    uv run run.py [--adapter <substring>] [--vulkan-sdk <dir>]

It runs on the first hardware adapter with 64-bit workgroup atomics, as the test does; `--adapter` narrows that by name.
It exits 1 when a row reads a wrong sum while the exchange-and-`or` row reads it right, 0 when every row is right, and 2 when the run is inconclusive.

## What we do about it in the tree

EMIT-258: SGL's SPIR-V stores a 64-bit word of workgroup memory with an `OpAtomicExchange` and loads it with an `OpAtomicOr` of 0, the last row above.
[bug-sgl-amd-vulkan-shared-64-bit-atomic-load-store](../../../libs/graphics/shaped-graphics-language/docs/issues/bug-sgl-amd-vulkan-shared-64-bit-atomic-load-store.md) tracks it.

## What to re-check after a driver update

Run `uv run run.py`; once it exits 0, SGL could write the plain forms again, after the CHK-510 test passes with them.
