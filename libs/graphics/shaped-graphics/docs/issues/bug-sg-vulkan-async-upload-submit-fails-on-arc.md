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
- **The VkResult is unknown**, since the assert does not print it; recording it is the first step, and tells device loss from an invalid submit.

## Hypotheses, unconfirmed

- The transfer queue family on this driver, contended by several contexts at once — the backend suite creates its own contexts beside the shared one.
- A device loss from an earlier submission, which the next submit is the first to see.
