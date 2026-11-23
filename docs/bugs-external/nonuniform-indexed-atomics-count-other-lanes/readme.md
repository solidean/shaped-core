# Intel and AMD: an atomic on a non-uniformly indexed buffer counts the lanes that picked another buffer

**Status:** open, not yet filed upstream with either vendor; worked around in SGL on Intel and AMD, by an sg feature only those adapters report.
**Affects:** Intel Arc B580 (driver 32.0.101.9150) and an AMD Radeon iGPU, RDNA 2 (driver 32.0.21042.62), on D3D12 and on vulkan alike, Windows 11; WARP is not affected.
**Found by:** `sg - an atomic update reaches the element of an SGL binding array a thread picks` in `shaped-graphics-test`, failing on every hardware arm since it landed in 16ac91582 (2026-11-22).
**Known elsewhere:** [R0Y00/Xbox2Windows#369](https://github.com/R0Y00/Xbox2Windows/issues/369), from 2026-11-21, has the same signature on RDNA 2 to 4 and on Intel UHD, and NVIDIA right.

## What happens

64 threads of one group each update the buffer of a two-element array that their parity picks, `slots[NonUniformResourceIndex(id.x % 2)]`.
Slot 0 of each buffer counts its threads with `InterlockedAdd`, and slot 1 keeps the least of their ids with `InterlockedMin`.
The right answer is `even 32 0, odd 32 1`.
Stripped of the matrix's variants, the shader is one line:

```hlsl
InterlockedAdd(slots[NonUniformResourceIndex(id.x % 2)][0], 1u);
```

- **Intel** adds every thread of the group into the even buffer, 64, and the odd threads into the odd buffer too, 32.
  So each buffer the wave updates in turn seems to take the add of every lane that has not had its turn yet; with four buffers that is 256, 192, 128, 64.
  Its min is right.
- **AMD** gives each buffer the whole wave's result: 64 adds in each, and a least id of 0 in each.
  So the wave's lanes seem to be combined into one atomic before they are split by the buffer they picked.
  Reading the atomic's previous value makes both right on this RDNA 2 driver, and not on the RDNA 4 drivers of the report above.

No call fails and no validation layer objects.
The DXIL marks the handle `nonUniformIndex`, and the SPIR-V decorates the access `NonUniform`, so both drivers are told that the index diverges.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library, and `vulkan/repro.cc` is the vulkan loader and the C++ standard library.
`shader.hlsl` is the shape of the HLSL SGL writes, compiled to DXIL by the Windows SDK's dxc 1.9.2602.17 and to SPIR-V by the Vulkan SDK's dxc 1.9.0.5512.
Each row changes one thing about the first; D3D12 and vulkan agree on every row:

```
configuration                                    Arc B580             Radeon iGPU          WARP
-----------------------------------------------  -------------------  -------------------  -----
add and min, results unread (what SGL writes)    even 64 0, odd 32 1  even 64 0, odd 64 0  right
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

## What a workaround has to look like

The rows after the bug's are shapes that might avoid it, each still picking the buffer by index.
They are information rather than a verdict, so `run.py` exits 1 on the bug's rows alone.
WARP is right on every one:

```
workaround                                          Intel D3D12  AMD D3D12  Intel vulkan  AMD vulkan
--------------------------------------------------  -----------  ---------  ------------  ----------
a loop over indices, leaving by break               right        right      wrong         wrong
  as above, unoptimized                             right        right      wrong         right
  as above, maximally reconverging                  right        right      right         wrong
a loop over indices, leaving by a flag              wrong        right      wrong         right
  as above, maximally reconverging                  wrong        right      wrong         right
  as above, the round's index unmarked              wrong        right      wrong         right
  as above, every round on the even buffer          right        right      right         right
one atomic per index, by a leader after the loop    wrong        wrong      wrong         wrong
  as above, maximally reconverging                  wrong        wrong      wrong         wrong
a compare-exchange by a leader in the round         right        right      wrong         wrong
  as above, unoptimized                             right        right      wrong         right
  as above, maximally reconverging                  right        right      right         right
the add natively by a leader in the round           right        right      wrong         wrong
  as above, maximally reconverging                  right        right      right         right
one lane at a time, natively                        wrong        wrong      wrong         wrong
one lane at a time within a round, natively         wrong        wrong      wrong         wrong
a compare-exchange per lane                         9 of 200     right      now and then  right
```

The rows read their atomics' values before as well, and each lane's is its own wherever the row is right.
A row Intel gets right once was run 200 times: the leaders in the round are right in every run, and a compare-exchange per lane is not.

- **A loop over the wave's distinct indices is no workaround.** Each round's lanes share one index, and still no loop shape is right on both vendors under vulkan.
  Intel is wrong in a round even with the index unmarked, as uniform as it is, and right only when every round updates one buffer.
  dxc's SPIR-V optimizer moves a `break` loop's atomic to the loop's merge block, where the lanes have reconverged; SPIR-V allows that without maximal reconvergence.
- **Neither is one lane at a time.** A loop in which each lane updates once, alone, means what every lane updating means, and is compiled as that.
- **One update per round, by one lane, is right everywhere.** The round's lanes combine their operands with subgroup reductions.
  Its first lane alone updates the element by the total, natively or by compare-exchange.
  It is one update per distinct index per wave, what a correct driver issues, and the round's prefix gives each lane its own value before.
  After the loop, the leaders hold distinct indices together, and their atomic is the bug again.
  On vulkan the loop needs maximal reconvergence, which both drivers expose as `VK_KHR_shader_maximal_reconvergence`; without it the reductions are taken over the reconverged wave.
- **A compare-exchange per lane is right on AMD, and on Intel in 191 of 200 runs.** A failing run's element holds one add too many, and a lane reads 32 before its own.
  So Intel seems to store a compare-exchange through a non-uniform handle now and then while reporting it failed, which the loop then retries.
  The same loop through literal indices is right in 200 of 200.

## Reproducing it

```bash
uv run run.py
```

It builds `repro.cc` and `vulkan/repro.cc` with `clang-cl`, and compiles `shader.hlsl` once per row, to DXIL and to SPIR-V.
`vulkan/repro.cc` enables maximal reconvergence where the device has it, which a shader then asks for by its execution mode.
Then it dispatches each row on every Intel, AMD or NVIDIA adapter it finds, and on WARP under D3D12.
It needs a Vulkan SDK for the vulkan arm, from `$VULKAN_SDK` or `--vulkan-sdk`, and skips that arm without one.
It exits 1 when an adapter's words are wrong on one of the bug's rows, and 2 when a run stops before its dispatch finishes.

## What shaped-core does about it

sg reports `feature::nonuniform_atomic_workaround` on Intel and AMD hardware, on dx12 and vulkan, and slib hands it to SGL with the context's other features.
Where the feature holds, SGL writes such an atomic as one update per round by a leader, natively, in a compute stage that has subgroups (CHK-561).
On vulkan that form needs `quad_control` too, whose extensions include maximal reconvergence, and the text states it.
Everywhere else, and for a `min`, `max` or bit operation whose value before is read, it writes a compare-exchange per lane, which Intel still gets wrong now and then.
A pixel stage keeps the native atomic, since its helper invocations would never leave such a loop.
Every other vendor keeps the native atomic and pays nothing.
`sg - an atomic at a nonuniform index into an SGL binding array is right in each form CHK-561 writes` runs each form on every device, the per-lane one on all but Intel.
[bug-sgl-binding-array-atomics-fail-on-gpu](../../../libs/graphics/shaped-graphics/docs/issues/bug-sgl-binding-array-atomics-fail-on-gpu.md) is the issue it closes.

## Re-checking after a driver update

Run `uv run run.py`.
An adapter with every bug row right is fixed: drop its vendor from `nonuniform_atomic_workaround` in sg's dx12 and vulkan backends, or bound it by the driver version that fixed it.
The test that found it is the check in the tree.
