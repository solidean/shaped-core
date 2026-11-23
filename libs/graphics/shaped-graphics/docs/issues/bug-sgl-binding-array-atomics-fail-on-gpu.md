---
created: 2026-11-23
refs:
  - test: sg - an atomic update reaches the element of an SGL binding array a thread picks
  - file: libs/graphics/shaped-graphics/tests/compute/compute-sync-test.cc
  - file: docs/bugs-external/nonuniform-indexed-atomics-count-other-lanes/readme.md
---

# An atomic update into an SGL binding array is wrong on Intel and AMD, a driver bug SGL does not yet write around

`sg - an atomic update reaches the element of an SGL binding array a thread picks` fails on dx12-hw, dx12-native-dxil, vulkan and vulkan-native-spirv, at `compute-sync-test.cc:215`.
The HLSL SGL writes is right: `InterlockedAdd(tallies_slots[NonUniformResourceIndex(id.x % 2)][0], 1u, ...)`, and WARP passes.
Both drivers miscompile an atomic on a non-uniformly indexed buffer, on D3D12 and on vulkan alike.
[nonuniform-indexed-atomics-count-other-lanes](../../../../../docs/bugs-external/nonuniform-indexed-atomics-count-other-lanes/readme.md) reproduces it standalone, with the matrix.

- Intel adds every lane of a wave into the first buffer it updates; its min is right.
- AMD combines the wave's lanes into one atomic before splitting them by buffer, for an add and a min alike, unless the shader reads the atomic's result.

What would close it: SGL writes an atomic through a binding array at a non-uniform index as a loop over the distinct indices the wave holds.
Each round, the lanes whose index equals the first remaining lane's update one buffer, uniform within the round.
The repro's branch to literal indices is right on both drivers, which is the shape such a loop gives each round.
Reading the result alone would fix AMD and not Intel.
Until then the test fails on both, and gating it on the two vendors is the other choice.

The SGL differential test that failed in the same run is [bug-sg-diff-half-corpus-fails-on-intel](bug-sg-diff-half-corpus-fails-on-intel.md): `half_arithmetic.sgl` alone, on Intel alone.
