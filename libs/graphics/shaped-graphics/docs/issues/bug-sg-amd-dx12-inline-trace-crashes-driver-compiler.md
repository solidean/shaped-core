---
created: 2026-11-13
tags: [amd, dx12, raytracing]
refs:
  - test: sg - an inline trace finds the nearest triangle, and the any-hit decision cuts one out
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/scene_traces.sgl
---

# On AMD's dx12 driver every inline trace crashes the driver's compiler

On AMD's Radeon iGPU (RDNA 2, dx12 driver 32.0.21042.62), creating a compute pipeline that traces inline ends the process with an access violation inside `amdxc64.dll`.
It never returns an error.
Seen 2026-11-13 under `SC_GPU=amd`: sg's own inline trace test and the differential corpus's `scene_traces.sgl` both end there, on the DXC route, which is the first one each creates.
It is not [amd-d3d12-dxil-without-llvm-ident-crashes](../../../../../docs/bugs-external/amd-d3d12-dxil-without-llvm-ident-crashes/readme.md): DXC's module carries `!llvm.ident`.

Not yet known: whether every RayQuery crashes it or something SGL's HLSL writes around one, and whether the vulkan driver traces where dx12 does not.
A standalone repro beside the other external bugs is the next step, then upstream.
Until then a full `shaped-graphics-test` run on that GPU ends at the first inline trace.
