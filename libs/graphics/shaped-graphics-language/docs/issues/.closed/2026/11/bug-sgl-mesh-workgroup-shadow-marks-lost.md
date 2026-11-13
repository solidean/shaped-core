---
created: 2026-11-09
refs:
  - file: docs/bugs-external/nvidia-mesh-groupshared-atomics-lost/readme.md
  - test: "sr - a mesh stage's read of workgroup memory no invocation stored is reported where it would have run (LEGAL-69)"
closed: 2026-11-09
resolution: wontfix
---

# A mesh stage's workgroup-memory shadow loses other invocations' marks on an RTX

The LEGAL-69 test, new in `08b6c1f9e`, fails on an RTX 5070 Ti on dx12 and vulkan, by DXC's route and the native one alike, and passes on WARP.
Invocation 2 stores nothing, so the one hole is `tile.values[2]`, which invocation 1 reads and which is reported as expected.
But invocations 0 and 2 also report `tile.values[1]` and `[3]`, which invocations 1 and 3 did store.

Measured on 2026-11-09:

* The data is right: a print after the barrier reads `10.0 11.0 0.0 13.0` in every invocation, so the stores of 1 and 3 land.
* Their shadow marks do not: the bit invocation 0 sets survives, and the bits of 1 and 3 are gone when 0 and 2 read them.
* The emitted HLSL is what EMIT and LEGAL-65 describe: invocation 0 clears `sgl_written`, a group barrier, each store `InterlockedOr`s its bit, a barrier, each read tests its bit.
* An extra `workgroup_barrier()` at the start of the body changes nothing, so it is not the clear racing the marks through the first barrier.
* The compute twin of the test, `shadow.sgl`, passes on the same GPU.

So the same text loses groupshared atomics of some lanes in a mesh stage on NVIDIA's driver, and keeps them on WARP.

Reduced on 2026-11-09 to twenty lines of HLSL through the stock DXC.
The reproduction is [nvidia-mesh-groupshared-atomics-lost](../../../../../../../../docs/bugs-external/nvidia-mesh-groupshared-atomics-lost/readme.md).
The trigger is the `aborted` flag the index guards set: a phi out of the divergent store branch, live across the barrier, read by the `return` after `SetMeshOutputCounts`.
Returning on the same condition spelled again, `-Od`, or the compute stage each keep every mark.

Left to decide: skip LEGAL-69 on NVIDIA, or end an aborted mesh invocation without a flag live across a barrier in what SGL emits.
