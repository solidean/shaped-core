# Intel: a process with an Agility SDK device recompiles its acceleration-structure kernels every run, on dx12 and vulkan

**Status:** open, not yet filed upstream and not yet reduced to a standalone reproduction; measured through sg on 2026-11-07.
**Affects:** Intel Arc B580, driver 32.0.101.9150, Windows 11, D3D12 Agility SDK 1.620.1.
**Found by:** `shaped-graphics-test` taking 182 s in `dev.py ship` against 9.9 s for the same binary built without the Agility SDK.

## What happens

Once a process has created a D3D12 device on the Agility SDK's runtime, each first acceleration-structure build of a kind costs 25–37 s of CPU in the driver's compiler.
The stacks sit in `igc-default64.dll` under `OpenCompilerDX_OCL`, reached from `OpenAdapter_Xe2` on dx12 and `OpenVulkanCompiler` on vulkan.
The second build of the same kind, on any device of the process, takes milliseconds.
It repeats in every new process: the driver rewrites its cache in `%USERPROFILE%\AppData\LocalLow\Intel\ShaderCache` (a 125 MB file there grows each run) but never reuses what it wrote.
The compiles serialize across the whole process, so every API's first build waits behind the one compiling.

The same compile is likely what [intel-d3d12-hit-object-pipeline-removes-device](../intel-d3d12-hit-object-pipeline-removes-device/readme.md) spends its 21 s in before the removal.

## The matrix

Each row is a fresh process building one two-triangle BLAS, then one TLAS over it.

| process | first build | time |
|---|---|---|
| dx12 on the system runtime | BLAS | 0.017 s |
| dx12 on the Agility runtime, debug layer off | BLAS | 26.3 s |
| dx12 on the Agility runtime, a second device after the first built | BLAS | 0.012 s |
| vulkan, no dx12 device in the process | BLAS, then TLAS | 0.001 s, 0.027 s |
| vulkan, after an Agility dx12 device that built nothing | TLAS | 36.5 s |
| vulkan, after an Agility dx12 device that built a BLAS | TLAS | 26.3 s |
| dx12 with `D3D12SDKVersion` exported and the DLLs beside the exe, instead of `CreateDeviceFactory` | BLAS | 26.2 s |

The one variable is whether the Agility runtime made a device in the process.
How it was loaded and whether the debug layer is on do not matter.

## Why it is not our bug

The builds are two triangles and one instance, and each is a single driver call; nothing of ours runs during the 26 s.
The system runtime builds the same input in 17 ms, and vulkan on its own in 27 ms.
A dx12 device changing what a vulkan device of the same process costs can only happen inside the vendor's shared compiler.

## What it costs us

- **Tests.** `shaped-graphics-test` pays it once per build kind per process: 182 s in the full run against 9.9 s without the Agility SDK.
  The same tax lands on `shaped-graphics-dx12-test`, `shaped-rendering-test` and `shaped-viewer-test`.
- **Applications.** Any process of ours that creates a dx12 device on this driver with the Agility SDK present should wait 26 s or more at its first trace build.
  `shaped-view` is expected to, but was not measured.

## What we do about it

Nothing yet.
The mitigation options are in [the sg issue](../../../libs/graphics/shaped-graphics/docs/issues/perf-sg-agility-intel-acceleration-structure-compile.md).

## Re-check when

Intel ships a driver after 32.0.101.9150, or the Agility SDK pin moves past 1.620.1.
The check is the first BLAS build of a fresh process on the Agility runtime: milliseconds means fixed.
