---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.cc:206
  - test: shaped-graphics-test
closed: 2026-11-06
resolution: fixed
---

# shaped-graphics-test segfaults in the NVIDIA Vulkan driver creating a ray-tracing pipeline

On Windows, relwithdebinfo-clang, an RTX 5070 Ti with driver 621.01, the whole shaped-graphics-test run dies with an access violation within seconds.
The faulting thread is in the NVIDIA driver (nvoglv64, then nvgpucomp64 or nvoglv64 itself), under the Khronos validation layer, called from `vulkan_raytracing_pipeline::create` at line 206.
The tests in flight at the time include "sg - an SGL hit object says what its closest hit is then handed" on the vulkan backend.
It reproduced on all three runs, one of them on 8c7b7c6ae, so it predates the clean-core change that was under test.

## Cause

The pipeline is `inspected_path`, whose raygen reads a hit object before invoking it.
SGL's `hlsl-vulkan` text copied the hit object into its `let` (`vk::SpirvOpaqueType<5313> h = hit_object_1;`), which DXC lowers to an `OpLoad` and `OpStore` of an `OpTypeHitObjectEXT` pointer.
SPV_EXT_shader_invocation_reorder forbids both, so the module was invalid, though neither spirv-val (Vulkan SDK 1.4.357) nor the validation layer reported it.
The driver's compiler crashed on it rather than rejecting it.

Fixed under EMIT-225: a hit object's `let` declares nothing and names the variable its trace filled.
`sgl target text - vulkan reorders through SPIR-V's own instructions` pins that no copy is written.
