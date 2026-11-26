# External bugs

Compiler and driver bugs we have hit, each with a **standalone reproduction** and what we concluded from it.

A bug lands here once we have stopped suspecting our own code.
Until then it is a finding about shaped-core and belongs in the library's own docs.

## Why a folder rather than a comment

A workaround in the tree says *what* we do about a bug.
It cannot say whether the bug is still there, whether it is ours, or what it takes to trigger.
Those questions come back every time a toolchain or a driver updates, and re-deriving the answer is the expensive part.

So each entry carries the answer as something runnable.

## When the evidence points outside our code

Driver and compiler bugs are common here, so this is the standing procedure rather than a judgment call each time.
Once the evidence points at a driver, a runtime or a compiler, the work is three steps, in order:

1. **Reduce it to a standalone repro** in this folder, by the rules below.
   An investigation that stops at "probably the driver" is unfinished: the repro is what settles it, and what a vendor report needs.
2. **Work around it or skip it, keyed to what was measured**: the vendor, the driver, the backend.
   A workaround stays as narrow as the evidence, and a test that cannot run skips through a known issue, never by commenting it out.
3. **Link the two** and record what to re-check after an update.

Where the repro will not reduce, the entry still lands with what was ruled out, marked as not reduced, and the issue stays open for it.

## The rules

- **One directory per bug**, kebab-case, named for the symptom rather than for the file that hit it.
- **`repro.cc`** (or the language's equivalent) depends on **nothing of ours**.
  No clean-core, no shaped-graphics, no `dev.py`, no third-party library beyond the one the bug is in.
  A reproduction that includes our headers cannot answer "is it us", which is the only question that matters.
- **`run.py`** drives the whole thing: finds the toolchain, builds, runs, prints a verdict.
  A `uv` shebang with PEP 723 metadata, matching [dev.py](../../dev.py); it must run from its own directory with no environment set up beforehand.
  Exit **1** when the bug reproduces and **0** when it does not, so it can be used as a check.
- **Minimize.** Cut until removing one more thing makes the bug disappear, then keep that thing and say why the bug needs it.
  A repro that is only *mostly* minimal hides the trigger.
- **`readme.md`** carries what happens and **why it is not our bug**, with the matrix that isolates the trigger.
  Then how to reproduce it, what we do about it in the tree, and what to re-check when the vendor ships an update.
- **Isolate the variable.** Two compilers, two optimization levels, two vendors' drivers — whatever separates "the tool is wrong" from "we are wrong".
  That table is usually the most valuable thing in the entry.
- **Link both ways.** The workaround in the tree names this directory; this directory names the workaround.
- **A driver bug a test trips over gets an entry in sg's [known_issues.cc](../../libs/graphics/shaped-graphics/src/shaped-graphics/context/known_issues.cc)**, keyed to what was measured.
  The test skips through `nx::skip_known_issue` under `sg::known_issue_affecting`, rather than spelling the condition out.
  `uv run dev.py test --known-issues` runs those tests anyway, and a pass says the entry may retire.
- **A bug a machine can carry gets a probe** in [known_issues.py](../../tools/dev/lib/toolchain/known_issues.py), when a version or a path gives it away without running anything.
  `dev.py doctor` then warns about it, and so does every command that runs our code, before the failure it explains.

## The entries

| bug | component | status |
|---|---|---|
| [msvc-const-ref-through-type-erased-call](msvc-const-ref-through-type-erased-call/readme.md) | MSVC `cl` 19.51 at `/O2` | filed upstream, quarantined in `function_ref-test.cc` |
| [msvc-coroutine-await-in-aggregate-init-crash](msvc-coroutine-await-in-aggregate-init-crash/readme.md) | MSVC `cl` 19.29 to 19.51 | not filed, compiles on the 19.52 preview, held off by the `co-await-in-initializer` lint rule |
| [vulkan-concurrent-device-lifecycle-deadlock](vulkan-concurrent-device-lifecycle-deadlock/readme.md) | NVIDIA Vulkan driver 591.86 | not yet filed, worked around in shaped-graphics |
| [nvidia-raytracing-device-lifecycle-cross-api-deadlock](nvidia-raytracing-device-lifecycle-cross-api-deadlock/readme.md) | NVIDIA driver 591.86, Vulkan and D3D12 | not yet filed, worked around in shaped-graphics |
| [nvidia-device-create-vs-pending-wait-deadlock](nvidia-device-create-vs-pending-wait-deadlock/readme.md) | NVIDIA Vulkan driver, RTX 5070 Ti | not yet filed, worked around in shaped-graphics |
| [nvidia-reorder-payload-qualifiers-across-pipelines](nvidia-reorder-payload-qualifiers-across-pipelines/readme.md) | NVIDIA D3D12 driver 621.01, RTX 5070 Ti, Agility SDK 1.620.1 | reproduced standalone, not yet filed, worked around in SGL's DXIL writer |
| [nvidia-mesh-groupshared-atomics-lost](nvidia-mesh-groupshared-atomics-lost/readme.md) | NVIDIA D3D12 driver 621.01, RTX 5070 Ti | reproduced standalone, not yet filed, its one test skipped on NVIDIA in shaped-rendering |
| [d3d12-cached-pso-static-sampler-mixup](d3d12-cached-pso-static-sampler-mixup/readme.md) | NVIDIA D3D12 driver 32.0.15.9186, RTX 5070 Ti | not yet filed, worked around in shaped-graphics |
| [intel-d3d12-cached-pso-draws-nothing](intel-d3d12-cached-pso-draws-nothing/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580 | not yet filed, its tests skipped in shaped-viewer |
| [intel-d3d12-view-instancing-draws-nothing](intel-d3d12-view-instancing-draws-nothing/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580 | not yet filed, worked around in the shaders that draw by view |
| [intel-d3d12-view-instanced-cached-blob-crashes](intel-d3d12-view-instanced-cached-blob-crashes/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580 | reproduced standalone, not yet filed, worked around in shaped-graphics |
| [intel-d3d12-strict-reciprocal-product-folds-to-one](intel-d3d12-strict-reciprocal-product-folds-to-one/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580 | not yet filed, its one check skipped in shaped-graphics |
| [intel-float-half-round-trip-folds-away](intel-float-half-round-trip-folds-away/readme.md) | Intel D3D12 and vulkan driver 32.0.101.9150, Arc B580 | not yet filed, logged through `half_math.sgl`'s `known` lines |
| [nonuniform-indexed-atomics-count-other-lanes](nonuniform-indexed-atomics-count-other-lanes/readme.md) | Intel driver 32.0.101.9150, Arc B580, and AMD driver 32.0.21042.62, Radeon iGPU (RDNA 2), D3D12 and vulkan | reproduced standalone, not yet filed, worked around in SGL where sg reports `nonuniform_atomic_workaround` |
| [intel-d3d12-hit-object-pipeline-removes-device](intel-d3d12-hit-object-pipeline-removes-device/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580, Agility SDK 1.620.1 | not yet filed, its one test skipped in shaped-graphics |
| [intel-agility-process-recompiles-acceleration-structure-kernels](intel-agility-process-recompiles-acceleration-structure-kernels/readme.md) | Intel driver 32.0.101.9150, Arc B580, Agility SDK 1.620.1, dx12 and vulkan | reproduced standalone, not yet filed, test processes on Intel keep to the system runtime |
| [amd-d3d12-dxil-without-llvm-ident-crashes](amd-d3d12-dxil-without-llvm-ident-crashes/readme.md) | AMD D3D12 driver 32.0.21042.62, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around in SGL's DXIL writer |
| [amd-d3d12-inline-ray-query-crashes-compiler](amd-d3d12-inline-ray-query-crashes-compiler/readme.md) | AMD D3D12 driver 32.0.21042.62, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around by signing SGL's DXIL |
| [amd-d3d12-negated-equality-branch-substitutes-constant](amd-d3d12-negated-equality-branch-substitutes-constant/readme.md) | AMD D3D12 driver 32.0.21042.62, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around in SGL's DXIL writer |
| [dxc-pixel-signature-packs-primitive-into-vertex-row](dxc-pixel-signature-packs-primitive-into-vertex-row/readme.md) | DXC 1.9.2602, drawn wrong by AMD D3D12 driver 32.0.21042.62, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around in SGL's DXIL, DXC's route a known issue on AMD |
| [intel-d3d12-mesh-table-without-root-parameters-hangs](intel-d3d12-mesh-table-without-root-parameters-hangs/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580 | reproduced standalone, not yet filed, worked around in sg's dx12 root signatures |
| [d3d12-mesh-pipeline-without-topology-refuses-its-blob](d3d12-mesh-pipeline-without-topology-refuses-its-blob/readme.md) | the D3D12 runtime, WARP, Intel and AMD alike | reproduced standalone, not yet filed, worked around in sg's dx12 mesh pipelines |
| [intel-vulkan-mixed-empty-hit-groups-crash-compiler](intel-vulkan-mixed-empty-hit-groups-crash-compiler/readme.md) | Intel Vulkan driver 1663934, Arc B580 | not reduced, not filed, worked around in sg's vulkan ray-tracing pipelines |
| [intel-d3d12-mesh-pipeline-built-after-a-restore-draws-nothing](intel-d3d12-mesh-pipeline-built-after-a-restore-draws-nothing/readme.md) | Intel D3D12 driver 32.0.101.9150, Arc B580 | reproduced standalone, not yet filed, worked around in sg's dx12 mesh pipelines |
| [amd-vulkan-cutout-trace-after-another-hangs](amd-vulkan-cutout-trace-after-another-hangs/readme.md) | AMD Vulkan driver 2.0.353, RDNA 2 iGPU | not reduced, not filed, skipped by a known issue in shaped-viewer's trace tests |
| [amd-vulkan-shared-64-bit-atomic-store-keeps-high-word](amd-vulkan-shared-64-bit-atomic-store-keeps-high-word/readme.md) | AMD vulkan driver 2.0.353, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around in SGL |
| [amd-vulkan-nonuniform-array-length-reads-first-lane](amd-vulkan-nonuniform-array-length-reads-first-lane/readme.md) | AMD vulkan driver 2.0.353, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around in SGL |
| [amd-vulkan-variable-count-layout-size-omits-binding](amd-vulkan-variable-count-layout-size-omits-binding/readme.md) | AMD vulkan driver 2.0.353, Radeon iGPU (RDNA 2) | reproduced standalone, not yet filed, worked around in shaped-graphics |
| [radv-aco-scheduler-wrong-bc7-indices](radv-aco-scheduler-wrong-bc7-indices/readme.md) | RADV, Mesa 26.2.0-devel, NAVI33 (RDNA 3) | root cause found, minimized, not yet filed, its tests skipped on that driver |
| [dxc-mesh-output-counts-duplicated-past-branch](dxc-mesh-output-counts-duplicated-past-branch/readme.md) | DXC 1.9.2602.24, 1.9.2602.17 and 1.9.0.5512, `ms_6_6` and above | reproduced standalone, not yet filed, a test shader written around it in shaped-graphics |
| [vulkan-syncval-wait-before-signal-false-race](vulkan-syncval-wait-before-signal-false-race/readme.md) | Khronos validation layer, SDK 1.4.304–1.4.341 | fixed upstream in 1.4.350, flagged by `dev.py doctor` |
| [webgpu-timestamp-write-index-sentinel](webgpu-timestamp-write-index-sentinel/readme.md) | emdawnwebgpu + Dawn, against wgpu | not yet filed, worked around in shaped-graphics |
| [deno-code-cache-stale-same-length-source](deno-code-cache-stale-same-length-source/readme.md) | Deno 2.9.6 | gone in 2.9.7, worked around in dev.py below it |
| [deno-wgpu-sample-mask-crash](deno-wgpu-sample-mask-crash/readme.md) | Deno 2.9.7's wgpu on vulkan, Intel Arc B580 | not filed, not worked around |
| [warp-arm64-pac-fastfail](warp-arm64-pac-fastfail/readme.md) | WARP on Windows 11 arm64 | trigger not yet isolated, worked around in shaped-graphics |
| [warp-raytracing-wrong-capture](warp-raytracing-wrong-capture/readme.md) | system WARP 10.0.26100.9711, x64 | gone in NuGet WARP 1.0.21, its test skipped on WARP in shaped-viewer |
| [libcxx-float-h-guard-vs-macos27-math-h](libcxx-float-h-guard-vs-macos27-math-h/readme.md) | Homebrew LLVM 22 libc++, macOS 27 SDK | fixed upstream in LLVM 23, worked around in the build |

## Retiring one

When a vendor ships a fix, `run.py` is what says so.
If it reports the bug gone on every configuration we care about, drop the workaround in the tree, its `known_issues.cc` entry and this directory with it, in the same change.
An entry nobody can retire because nobody knows what "fixed" looks like is the failure this format exists to prevent.
