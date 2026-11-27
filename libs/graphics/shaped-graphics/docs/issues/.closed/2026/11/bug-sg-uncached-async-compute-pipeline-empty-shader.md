---
created: 2026-11-11
refs:
  - symbol: sg::context_uncached_scope::create_compute_pipeline_async
  - symbol: sg::context::create_compute_pipeline_async
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/uncached.cc
closed: 2026-11-27
resolution: fixed
---

# `ctx.uncached.create_compute_pipeline_async` can hand the backend an empty or corrupted shader

Found while building a polyfilled mesh stage's compute half on vulkan (2026-11-11).
Built through `ctx.uncached.create_compute_pipeline_async`, the backend's `vulkan_compute_pipeline::create` met a shader with no bytecode.
With the validation layer on, it met a `vkCreateShaderModule` whose `codeSize` read as a code address instead.
The same description built through `ctx.cached.acquire_compute_pipeline` builds every time, which is what the polyfill uses now.

Measured, not explained:

- The description was intact where it was handed over: its entry point, its stage and its SPIR-V magic, logged in slib, in `create_compute_pipeline_async` and at the backend's create.
- It failed on the first iteration of one test alone, `sg - a polyfilled SGL mesh stage draws its triangle*` on the vulkan context, and passed with logging in between, so timing changes it.
- `context::create_compute_pipeline_async` copies the description into its build lambda, as `create_raster_pipeline_async` does, and the raster one has never shown this.

Next: a test that builds one compute pipeline through `ctx.uncached.create_compute_pipeline_async` and awaits it, on every backend, under `--repeat`.

## Resolution

`compute_pipeline_description` holds its shader as `compiled_shader const&`, where a raster description holds its stages by value.
`context::create_compute_pipeline_async` copied the description into its build, which copied the reference, and the build runs on a worker after the call returns.
So it read whatever the caller's shader had become by then: in the polyfill, a description local to the raster call that had already gone.
The build now owns a copy of the shader.
`sg - an async compute build owns its shader, so the caller's may change or go at once` empties the caller's copy after the call; it failed with "compute shader has no bytecode" before.
