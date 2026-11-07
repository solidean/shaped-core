---
created: 2026-11-06
---

# A device loss on the Arc under a repeated sg suite, which an assert turns into an abort

`uv run dev.py test shaped-graphics-test --repeat 20` lost the vulkan device once, on iteration 8, on 2026-11-06 on the Intel Arc B580.
A rerun of 12 iterations passed, and no Windows driver-timeout event (4101) was logged, so nothing shows what lost it.
The test running at the time was `sg - a strong compare-exchange in a pixel stage leaves helper pixels out, and the draw ends`, which is only where the loss was noticed.
Separately, `vulkan_upload_async.cc` asserts on `vkQueueSubmit` rather than marking the device lost, so a loss there aborts the binary instead of failing the test.

## Seen again on 2026-11-07

`--repeat 8` crashed on iteration 7 on the Arc B580, with `print` and `assert` lowered in legalize (43b1753d4) and geometry and tessellation merged.

- **It is the GPU rather than vulkan.** vulkan logged `VK_ERROR_DEVICE_LOST`, and within 0.2 s dx12's debug layer logged `DXGI_ERROR_DEVICE_HUNG` in the same process.
- **`compute-float-specials-test.cc` read zeros on every line**, which is a dispatch that never ran rather than a float bug; it is one more place the loss was noticed.
- **The abort is still the assert in `vulkan_upload_async.cc`**, `vkQueueSubmit (async upload) failed`.

**The rate is not settled.**
Across these runs, 3 of about 34 iterations crashed with the legalize change or its fallback in the tree, against 1 of 32 before it, and those counts are too few to say the change raised it.
Measuring it means a long repeat on each side, one run at a time.
No hung submission has been identified, so what hangs the GPU is unknown.
