---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.cc:206
  - test: shaped-graphics-test
---

# shaped-graphics-test segfaults in the NVIDIA Vulkan driver creating a ray-tracing pipeline

On Windows, relwithdebinfo-clang, an RTX 5070 Ti with driver 621.01, the whole shaped-graphics-test run dies with an access violation within seconds.
The faulting thread is in the NVIDIA driver (nvoglv64, then nvgpucomp64 or nvoglv64 itself), under the Khronos validation layer, called from `vulkan_raytracing_pipeline::create` at line 206.
The tests in flight at the time include "sg - an SGL hit object says what its closest hit is then handed" on the vulkan backend.
It reproduced on all three runs, one of them on 8c7b7c6ae, so it predates the clean-core change that was under test.
Not yet known: which pipeline triggers it, whether it happens without the validation layer, and whether another driver version does it too.
