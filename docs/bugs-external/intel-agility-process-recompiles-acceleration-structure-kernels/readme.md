# Intel: a process that loads the Agility SDK's device factory never reads its acceleration-structure compile from the driver's cache

**Status:** open, reproduced standalone, not yet filed upstream, worked around in our test processes on Intel; measured on 2026-11-07.
**Affects:** Intel Arc B580, driver 32.0.101.9150, Windows 11, D3D12 Agility SDK 1.620.1; dx12 and vulkan alike.
**Found by:** `shaped-graphics-test` taking 182 s in `dev.py ship` against 9.9 s for the same binary built without the Agility SDK.

## What happens

The first acceleration-structure build of a process costs a 20–30 s compile in the driver.
It is paid inside the call that records the build, `BuildRaytracingAccelerationStructure` or `vkCmdBuildAccelerationStructuresKHR`, on the CPU.
The GPU work after it takes a millisecond.
It is paid once per process, whatever is built first: every later build, of any kind and on either API, costs nothing.

Normally that compile lands in the driver's cache, per application, and every later process reads it back in milliseconds.
A process that has called the Agility SDK's `CreateDeviceFactory` never reads it back.
It compiles again every run, and the cache file it writes to (in `%USERPROFILE%\AppData\LocalLow\Intel\ShaderCache`) grows by about 100 KB each time.
That holds for its vulkan devices too, which share the driver's compiler (`igc-default64.dll`) with dx12.

The same compile is likely what [intel-d3d12-hit-object-pipeline-removes-device](../intel-d3d12-hit-object-pipeline-removes-device/readme.md) spends its 21 s in before the removal.

## The trigger

Each row is a fresh process of the same executable, with the driver's cache already warm for it.
The time is what its first build cost, from the prebuild query to the fence.

| process | first build | time |
|---|---|---|
| dx12, system runtime | BLAS | 0.012 s |
| dx12, Agility runtime | BLAS | 23.5 s, then 32.0 s in the next process |
| dx12, Agility runtime | TLAS over a null instance | 21.8 s |
| dx12, Agility runtime: PREFER_FAST_BUILD, then PREFER_FAST_TRACE, ALLOW_UPDATE and a TLAS | the first one | 21.5 s, the other three 0 s |
| vulkan alone | BLAS | 0.015 s |
| vulkan, after an Agility dx12 device | BLAS | 26.8 s |
| vulkan, after an Agility dx12 device that was released again | BLAS | 18.8 s |
| vulkan, after `CreateDeviceFactory` and no dx12 device | BLAS | 18.8 s, 20.1 s |
| vulkan, after `D3D12GetInterface(CLSID_D3D12SDKConfiguration)` and no factory | BLAS | 0.013 s |
| vulkan, after `LoadLibrary` of the Agility `D3D12Core.dll` and nothing else | BLAS | 0.014 s |
| vulkan, after `LoadLibrary` of the system `D3D12Core.dll` | BLAS | 0.015 s |
| AMD Radeon iGPU (RDNA 2, 32.0.21042.62), dx12, system runtime | BLAS | 0.001 s |
| AMD Radeon iGPU, dx12, Agility runtime | BLAS | 0.001 s |

The two AMD rows are the control, from `run.py --vendor 1002` on the same machine: AMD's driver keeps its cache on the Agility runtime.

So the bug needs exactly two things:

- **`CreateDeviceFactory` on the Agility runtime.** Not a device, and not the DLL merely being loaded: the factory initializing the runtime is the step that turns the cache off.
  Releasing everything afterwards does not turn it back on.
- **An acceleration-structure build.** The prebuild query costs nothing, and so does creating either API's device.
  The build kind and its flags do not matter.

In the factory-only process no Intel dx12 driver DLL is loaded at all; its module list differs from the vulkan-alone one by the Agility `D3D12Core.dll` alone.
So the vulkan driver itself reacts to the Agility runtime having initialized in the process.
How it notices is the vendor's question, not one this reproduction can answer.

A new application misses the cache once on every runtime: the first `repro.exe` paid 18.7 s on the system runtime, and 29.8 s on vulkan.
Rebuilding it, and moving it to another directory, did not miss again, so the driver keys its cache on something like the executable's name rather than its path or contents.
`run.py` warms it with a first system-runtime process for that reason, and reads its verdict off the second.

## Why it is not our bug

`repro.cc` is D3D12, DXGI, the Agility SDK, the Vulkan loader and the C++ standard library.
Its builds are one triangle and one instance, each a single driver call, and nothing of ours runs during the 20 s.
The system runtime builds the same input in 12 ms, and vulkan on its own in 15 ms.
A D3D12 runtime changing what a vulkan device of the same process costs, with no D3D12 device in the process, can only happen inside the vendor's driver.

## Reproducing it

    uv run run.py [--agility <install>] [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

It builds `repro.cc` with clang-cl into one stable directory under the temp folder.
`--agility` is a directory holding the Agility SDK's `include/` and `bin/`; it defaults to this repository's `extern/agility/.install`.
The vulkan arms need the Vulkan SDK's headers and are skipped without them; the loader comes from the system.
`--vendor` runs every arm on the first adapter of one PCI vendor, `1002` for AMD, instead of the high-performance one.
It exits 1 when the first BLAS costs seconds on the Agility runtime and milliseconds on the system runtime, and 0 when both are fast.
A run takes about a minute, since the Agility arms pay the compile each time.
On the machine above it prints, phases trimmed:

```
dx12, system runtime:      dx12.blas.record=0.011
dx12, Agility runtime:     dx12.blas.record=22.315
vulkan alone:              vulkan.blas.record=0.014
vulkan after the Agility device factory, no dx12 device:
                           dx12.factory=0.002 vulkan.blas.record=19.426
first BLAS: 0.012 s on the system runtime, 22.316 s on the Agility runtime
REPRODUCED: a process on the Agility runtime compiles for seconds at its first acceleration-structure build
```

`repro.exe` also takes the arms by hand, for the rows above: `--agility <bin>`, then `--dx12 <op>` and `--vulkan <op>` in order.
The header comment of `repro.cc` lists the ops.

## Related public report

[IGCIT #1548](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1548) is the closest one found.
It is on vulkan, on an Arc B580: Counter-Strike 2 with GPU ray tracing stops responding 23–38 s after launch, stuck in `igc-default64.dll`.
It names driver 32.0.101.8629 as good and 32.0.101.8992 as bad, and Intel tracks it internally as 22022863112.
The reporter's diff of the two drivers found BVH builder code newly in `igvk64.dll`.

Ours differs: the delay ends, it needs the Agility runtime, and it is an acceleration-structure build rather than a pipeline compile.
A shared root cause, a builder kernel the driver compiles at run time since that change, is a hypothesis only.

## What we do about it

**On an Intel GPU, every process of ours keeps to the system runtime unless asked otherwise**, marked `TEMPORARY(debt-sg-intel-keeps-off-agility)`.
`dx12::requested_d3d12_runtime()` makes the choice through DXGI alone, before any D3D12 runtime is loaded.
[dx12_runtime.cc](../../../libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_runtime.cc) holds it.
`shaped-graphics-test` runs the tests that need the Agility SDK in a process of its own, which pays the compile once and in parallel.
[dx12-agility-process.cc](../../../libs/graphics/shaped-graphics/tests/backends/dx12-agility-process.cc) lists them and starts it.
[The debt issue](../../../libs/graphics/shaped-graphics/docs/issues/debt-sg-intel-keeps-off-agility.md) says what coverage that moves, and when to remove it.

Applications, `shaped-view` included, so run without SM 6.9, shader execution reordering and DXR 1.2 there.
`SC_DX12_RUNTIME=agility`, or `dx12::choose_d3d12_runtime` before the first device, asks for the Agility SDK anyway and pays the compile.

What it costs, measured on the machine above on 2026-11-07, each binary alone:

| binary | Agility SDK in every process | system runtime on Intel |
|---|---|---|
| `shaped-graphics-test` | over 175 s | 23.6 s, its Agility process 23 s of that in parallel |
| `shaped-graphics-dx12-test` | 85 s | 1.7 s |
| `shaped-rendering-test` | 64 s | 11.7 s |
| `shaped-viewer-test` | 63 s | 45 s |
| `shaped-shader-compiler-dxc-test` | 25 s | 0.55 s |

**Applications.** Any process of ours on this driver with the Agility SDK present should wait about 20 s at its first trace.
`shaped-view` is expected to, but was not measured.

## Draft upstream report

Not posted; it is here for whoever files it at the [IGCIT tracker](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues).

> **Title:** Process using the D3D12 Agility SDK never reuses the cached ray-tracing acceleration-structure compile (20–30 s per process, dx12 and Vulkan)
>
> **System:** Arc B580, driver 32.0.101.9150, Windows 11 (10.0.26300), D3D12 Agility SDK 1.620.1.
>
> **Problem:** the first acceleration-structure build in a process spends 20–30 s in `igc-default64.dll`, inside the call that records it.
> Without the Agility SDK, the compile is cached and every later run of the same executable takes about 12 ms.
> Once the process has called `ID3D12SDKConfiguration1::CreateDeviceFactory` for the Agility SDK, the cache is never read: every run compiles again and the cache file grows.
> This affects Vulkan as well: a Vulkan BLAS build in a process that created the Agility device factory, and no D3D12 device, takes 19–20 s instead of 15 ms.
> Loading the Agility `D3D12Core.dll` without calling `CreateDeviceFactory` does not trigger it.
>
> **Reproduction:** one C++ file, attached, building one triangle BLAS.
> `repro.exe --agility <dir> --dx12 blas` takes 20+ s every run; `repro.exe --dx12 blas` takes 0.01 s after its first run.
> `repro.exe --agility <dir> --dx12 factory --vulkan blas` takes 20 s every run; `repro.exe --vulkan blas` takes 0.015 s.
>
> **Possibly related:** #1548, also `igc-default64.dll` on ray tracing on the B580.
>
> **Impact:** every application shipping the Agility SDK pays 20–30 s on each launch before its first ray-traced frame.

## Re-check when

Intel ships a driver after 32.0.101.9150, or the Agility SDK pin moves past 1.620.1: run `uv run run.py`, and once it exits 0, close the sg issue.
Trying driver 32.0.101.8629, the last good one of IGCIT #1548, would say whether the two share the regression.
