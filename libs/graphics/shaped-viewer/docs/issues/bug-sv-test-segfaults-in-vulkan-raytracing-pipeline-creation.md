---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.cc
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/raytracing_pipeline.cc
  - test: sv vulkan - the path tracer traces what it traces on dx12, to rounding
---

# shaped-viewer-test segfaults creating a vulkan ray-tracing pipeline, at origin/main

`shaped-viewer-test` dies with an access violation about 6 s in, on every run here, at `origin/main` 0507d7ace.
Measured on an Arc B580 (vulkan driver 1663934, dx12 32.0.101.9150), relwithdebinfo, 2026-11-13: 3 runs of 3 crashed, one of them with the tree at plain `origin/main`.

The faulting stack is a pool worker in `sg::backend::vulkan::vulkan_raytracing_pipeline::create`, under `context_uncached_scope::try_create_raytracing_pipeline`.
Another run resolved its sites to `slib::compile_hit_group`'s `fail` lambda building a `pipeline_creation_exception`, inside `async_error_from_exception_trait::make`.
The tests in flight each time were the cross-backend ones that make their own vulkan context:

- `sv vulkan - the path tracer traces what it traces on dx12, to rounding`
- `sv vulkan - every mode and every denoiser draws the view it draws on dx12`
- `sv - the path tracer through SGL's binary traces what DXC's traces, on dx12 and on vulkan`
- `sv::pathtrace_routine - a material that does not compile costs its own meshes, not the view`

The last one compiles a hit group that fails on purpose, which is the `compile_hit_group` site.
So the hypothesis, not traced: a failing hit-group compile and a vulkan pipeline build race on something they share.

`dev.py ship`'s proof judged every sv test "green before the delta" on this machine, though the base crashes.
That verdict came from a stored result rather than a run, and blocks every push from here until this is fixed.
