---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_upload_async.cc
  - test: shaped-graphics-vulkan-test
---

# vkQueueSubmit (async upload) fails under the full vulkan backend suite on an Intel Arc B580

`uv run dev.py test shaped-graphics-vulkan-test` aborts within seconds, every run so far, on `CC_ASSERT(r == VK_SUCCESS, "vkQueueSubmit (async upload) failed")` in `vulkan_upload_async.cc`.
The machine is Windows 11 with an Intel Arc B580 beside an AMD iGPU, Vulkan SDK 1.4.363.1; it had never run the vulkan backend before.

## Known

- **It predates the change that found it.** The backend files reverted to before `feat-sg-external-device-requirements` abort the same way.
- **Which test is running varies** — a ray-tracing one, a draw over a copied array, a procedural blas — so the submit fails under load rather than for one test's sake.
- **Each test passes alone**: `vulkan-raytracing-test.cc` by itself passes, in 21 s.
- **The cross-backend suite does not hit it**: `shaped-graphics-test`'s vulkan arm passes all of its tests on the same machine and device.
- **It is a device loss.** The suite's own check reports `VK_ERROR_DEVICE_LOST` from the completion signal wait (`vulkan-entry.cc`), so the failing submit is only the first call to see it.

- **On the AMD iGPU beside it** (`VK_LOADER_DRIVERS_SELECT=*amd*`) the suite does not crash.
  It found a real bug instead: a TLAS build's instance input was placed at the memory requirement's alignment, 4 there, where Vulkan demands 16.
  That is fixed (`vulkan_context::vk_buffer_alignment`), and the Arc still loses its device afterwards, so it was not this.
- The iGPU's one remaining failure is a negative timestamp delta of exactly −2³² ticks: `bug-sg-vulkan-timestamp-wraps-on-amd-igpu`.

## Next

- Find the submission that hangs the GPU: run the suite's tests one at a time with `--repeat`, or under the validation layer's GPU-assisted mode.
- Hypothesis, unconfirmed: ray tracing on this driver, since the slow ray-tracing tests (21 s alone) are the likeliest to trip a TDR when other work shares the GPU.
