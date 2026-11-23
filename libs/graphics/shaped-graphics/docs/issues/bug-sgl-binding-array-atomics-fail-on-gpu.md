---
created: 2026-11-23
refs:
  - test: sg - an atomic update reaches the element of an SGL binding array a thread picks
  - file: libs/graphics/shaped-graphics/tests/compute/compute-sync-test.cc
  - file: docs/bugs-external/nonuniform-indexed-atomics-count-other-lanes/readme.md
---

# An atomic update into an SGL binding array is wrong on Intel and AMD, a driver bug SGL writes around

`sg - an atomic update reaches the element of an SGL binding array a thread picks` failed on dx12-hw, dx12-native-dxil, vulkan and vulkan-native-spirv, on Intel and AMD.
The HLSL SGL writes is right: `InterlockedAdd(tallies_slots[NonUniformResourceIndex(id.x % 2)][0], 1u, ...)`, and WARP passes.
Both drivers miscompile an atomic on a non-uniformly indexed buffer, on D3D12 and on vulkan alike.
[nonuniform-indexed-atomics-count-other-lanes](../../../../../docs/bugs-external/nonuniform-indexed-atomics-count-other-lanes/readme.md) reproduces it standalone.
Its matrix shows what a workaround has to look like.

What closed it is SGL's CHK-553, where sg reports `feature::nonuniform_atomic_workaround`, which only Intel and AMD hardware do.
There such an atomic is one native update per round by a leader, in a compute stage.
Its vulkan text states maximal reconvergence (EMIT-276), and SGL's own DXIL keeps a break's blocks in their loop as DXC does with `@dx.break.cond`.
The forms CHK-553 does not reach yet are [debt-sgl-nonuniform-atomic-workaround-gaps](debt-sgl-nonuniform-atomic-workaround-gaps.md).

The SGL differential test that failed in the same run is [bug-sg-diff-half-corpus-fails-on-intel](bug-sg-diff-half-corpus-fails-on-intel.md): `half_arithmetic.sgl` alone, on Intel alone.
