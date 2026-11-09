---
created: 2026-11-09
refs:
  - test: "sr - a mesh stage's read of workgroup memory no invocation stored is reported where it would have run (LEGAL-69)"
---

# A mesh stage's workgroup-memory shadow loses other invocations' marks on an RTX

What is wrong or wanted, and what is already known.

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
Hypothesis only: NVIDIA's mesh stage keeps groupshared and its outputs in one allocation, and an output write or the lanes past the four declared ones disturb `sgl_written`.
Next: the DXIL and the SPIR-V side by side with the compute twin's, and the same test on an AMD or Intel GPU.
