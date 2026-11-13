---
created: 2026-11-13
tags: [amd, dx12, raytracing, dxil]
refs:
  - test: sg - an inline trace finds the nearest triangle, and the any-hit decision cuts one out
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/scene_traces.sgl
closed: 2026-11-13
resolution: fixed
---

# On AMD's dx12 driver an inline trace of SGL's native DXIL crashes the driver's compiler

On AMD's Radeon iGPU (RDNA 2, dx12 driver 32.0.21042.62), creating a compute pipeline that traces inline ends the process with an access violation inside `amdxc64.dll`.
It is SGL's native DXIL that crashes, not DXC's: a native container is unsigned, and AMD's shader cache crashes on an unsigned ray query once an unsigned plain shader is in it.
[amd-d3d12-inline-ray-query-crashes-compiler](../../../../../../../../docs/bugs-external/amd-d3d12-inline-ray-query-crashes-compiler/readme.md) reproduces it standalone, with DXC's modules stamped unsigned.

A full `shaped-graphics-test` run on that GPU compiles plain native shaders first, so it ends at its first inline trace.
Resetting the driver's shader cache helps until the next plain shader.

The fix in our hands is to sign SGL's containers: the digest is DXBC's checksum, which the writer can compute itself.
Each native container then has its own digest, so no two share the driver's cache entry, and the crash has no unsigned container to meet.
It would also retire whatever of [amd-d3d12-dxil-without-llvm-ident-crashes](../../../../../../../../docs/bugs-external/amd-d3d12-dxil-without-llvm-ident-crashes/readme.md) was this cache.

## Log

- **2026-11-13: fixed by signing SGL's containers.**
  The DXIL writer stamps each container with DXBC's checksum, so no two native shaders share the driver's cache entry.
  sg's inline trace test and the whole differential corpus run on that GPU now, with the cache that crashed them still in place.
