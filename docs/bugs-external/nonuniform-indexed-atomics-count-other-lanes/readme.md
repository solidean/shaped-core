# Intel and AMD: an atomic on a non-uniformly indexed buffer counts the lanes that picked another buffer

**Status:** open, not yet filed upstream with either vendor; not worked around, so its one test fails on both.
**Affects:** Intel Arc B580 (driver 32.0.101.9150) and an AMD Radeon iGPU, RDNA 2 (driver 32.0.21042.62), on D3D12 and on vulkan alike, Windows 11; WARP is not affected.
**Found by:** `sg - an atomic update reaches the element of an SGL binding array a thread picks` in `shaped-graphics-test`, failing on every hardware arm since it landed in 16ac91582 (2026-11-22).

## What happens

64 threads of one group each update the buffer of a two-element array that their parity picks, `slots[NonUniformResourceIndex(id.x % 2)]`.
Slot 0 of each buffer counts its threads with `InterlockedAdd`, and slot 1 keeps the least of their ids with `InterlockedMin`.
The right answer is `even 32 0, odd 32 1`.

- **Intel** adds every thread of the group into the even buffer, 64, and the odd threads into the odd buffer too, 32.
  So the first buffer a wave updates seems to take the add of every active lane, and the next one only its own.
  Its min is right.
- **AMD** gives each buffer the whole wave's result: 64 adds in each, and a least id of 0 in each.
  So the wave's lanes seem to be combined into one atomic before they are split by the buffer they picked.
  Reading the atomic's previous value makes both right, presumably because the driver then takes another path for it.

No call fails and no validation layer objects.
The DXIL marks the handle `nonUniformIndex`, and the SPIR-V decorates the access `NonUniform`, so both drivers are told that the index diverges.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library, and `vulkan/repro.cc` is the vulkan loader and the C++ standard library.
`shader.hlsl` is the shape of the HLSL SGL writes, compiled to DXIL by the Windows SDK's dxc 1.9.2602.17 and to SPIR-V by the Vulkan SDK's dxc 1.9.0.5512.
Each configuration changes one thing about the first row; D3D12 and vulkan agree on every row:

```
configuration                                    Arc B580             Radeon iGPU          WARP
-----------------------------------------------  -------------------  -------------------  -----------------
add and min, results unread (what SGL writes)    even 64 0, odd 32 1  even 64 0, odd 64 0  even 32 0, odd 32 1
add alone                                        even 64 -, odd 32 -  even 64 -, odd 64 -  right
min alone                                        right                even 0 0, odd 0 0    right
add and min, results read                        even 64 0, odd 32 1  right                right
add alone, its result read                       even 64 -, odd 32 -  right                right
add and min, at shader model 6.8                 even 64 0, odd 32 1  even 64 0, odd 64 0  right
add and min, unoptimized by dxc                  even 64 0, odd 32 1  even 64 0, odd 64 0  right
the buffer picked by a branch, literal indices   right                right                right
```

`-` is a word the row leaves alone.
The same buffers picked by a branch on the parity, each indexed by a literal, are right everywhere, so the arithmetic of the shader is not at fault.
WARP is right on every row, and dxc at `-Od` changes nothing, so the input each driver gets is what the shader says.

## What the bug needs

- **A buffer picked by a non-uniform index.** A branch to literal indices is right on both.
- **An atomic whose result is unread, on AMD.** Either atomic, add or min, goes wrong alone.
- **The add, on Intel**, read or not; its min is right.
- **Nothing of the toolchain.** Shader model 6.0 and 6.8, optimized or not, D3D12 or vulkan.

## Reproducing it

```bash
uv run run.py
```

It builds `repro.cc` and `vulkan/repro.cc` with `clang-cl`, and compiles `shader.hlsl` once per row, to DXIL and to SPIR-V.
Then it dispatches each row on every Intel, AMD or NVIDIA adapter it finds, and on WARP under D3D12.
It needs a Vulkan SDK for the vulkan arm, from `$VULKAN_SDK` or `--vulkan-sdk`, and skips that arm without one.
It exits 1 when an adapter's words are wrong, and 2 when a run stops before its dispatch finishes.

## What shaped-core does about it

Nothing yet, so the test that found it fails on Intel and AMD.
SGL could write what is right on both: a loop over the distinct indices a wave holds, each round's lanes updating one buffer that is uniform within the round.
That is a choice about what SGL emits for every atomic through a binding array, rather than a local fix.
It is open in [bug-sgl-binding-array-atomics-fail-on-gpu](../../../libs/graphics/shaped-graphics/docs/issues/bug-sgl-binding-array-atomics-fail-on-gpu.md).

## Re-checking after a driver update

Run `uv run run.py`.
An adapter with every row right is fixed; the test that found it is the check in the tree.
