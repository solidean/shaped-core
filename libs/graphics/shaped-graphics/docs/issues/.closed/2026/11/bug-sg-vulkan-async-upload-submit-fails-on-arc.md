---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_upload_async.cc
  - test: shaped-graphics-vulkan-test
closed: 2026-11-05
resolution: fixed
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

## Cause (2026-11-05)

- **A vacant array element was written as all-zero bytes, which is not Intel's null descriptor.**
  The test that loses the device alone is `sg vulkan - a draw with an undeclared array binding logs once, and the draw runs`, a draw through a table of four vacant elements.
  Running each test file alone, then each test of `vulkan-raster-test.cc`, found it; "each test passes alone" above had sampled the wrong file.
- VK_EXT_descriptor_buffer's null descriptor is what `vkGetDescriptorEXT` returns for no resource, which the nullDescriptor feature makes a valid request.
  Its bytes are the driver's: NVIDIA and RADV answer zeros, so sg's zeros worked there, and the Arc reads a zeroed descriptor as a real one.
- **Fixed** by `write_null_descriptor`, which asks the driver, in the binding group's vacant elements and in a staging group's initial and cleared descriptors.
  The vulkan backend suite then passes on the Arc, three runs in a row.
- The ray-tracing hypothesis was wrong.
