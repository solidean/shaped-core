---
created: 2026-11-06
---

# A device loss on the Arc under a repeated sg suite, which an assert turns into an abort

`uv run dev.py test shaped-graphics-test --repeat 20` lost the vulkan device once, on iteration 8, on 2026-11-06 on the Intel Arc B580.
A rerun of 12 iterations passed, and no Windows driver-timeout event (4101) was logged, so nothing shows what lost it.
The test running at the time was `sg - a strong compare-exchange in a pixel stage leaves helper pixels out, and the draw ends`, which is only where the loss was noticed.

## Seen again on 2026-11-07

`--repeat 8` crashed on iteration 7 on the Arc B580, with `print` and `assert` lowered in legalize (43b1753d4) and geometry and tessellation merged.

- **It is the GPU rather than vulkan.** vulkan logged `VK_ERROR_DEVICE_LOST`, and within 0.2 s dx12's debug layer logged `DXGI_ERROR_DEVICE_HUNG` in the same process.
- **`compute-float-specials-test.cc` read zeros on every line**, which is a dispatch that never ran rather than a float bug; it is one more place the loss was noticed.
- **The abort was the assert in `vulkan_upload_async.cc`** at that time, `vkQueueSubmit (async upload) failed`; see below.

**The legalize change did not raise the rate, as far as a comparison can show.**
Across those earlier runs, 3 of about 34 iterations crashed against 1 of 32, which was too few to read.
So `--repeat` alternated 3b9189646, before native `print`, with f7232f75a, which also holds mesh and task, four rounds of each.

| side | iterations counted | device-loss aborts |
|---|---|---|
| before | 40 | 2 |
| after | 40 | 2 |

Each side also had two rounds cut off by a 175 s cap and not counted.
Those rounds logged no crash, and no 20-iteration run fitted the cap, so the cap rather than a hang is the likely reason.
All four aborts are `vkQueueSubmit` failing or `VK_ERROR_DEVICE_LOST`, at different tests each time, so the test running is where the loss is noticed rather than what causes it.
This is a rate of very roughly one iteration in twenty to forty, so one `dev.py ship` meets it now and then.
No hung submission has been identified, so what hangs the GPU is unknown.

## What a loss does in an async transfer now

An async upload or download whose `vkQueueSubmit` loses the device marks it lost and settles the job as cancelled, where it used to assert.
A download must not go on, since its staging window holds none of the job's bytes and would be delivered as a success.
Forcing `VK_ERROR_DEVICE_LOST` on the fifth submit of each system shows the loss reaching tests as `sg::device_lost_exception`, named `vkQueueSubmit (async upload)` or `(async download)`.
Nothing keeps that under test: no seam can fail a submit, so the check was a temporary mutation.

## Left open

- **The process still aborts after a loss.** Under that forced loss, the suite failed its tests normally and then exited 0xC0000409, right after `vulkan-entry.cc`'s `is_device_lost` check.
  The real crash `063051` ended the same way with no assert at all, so this path may be the commoner one.
  The crash report of the forced runs was empty, so what aborts is unknown; context teardown is the suspect, and `shutdown_no_throw` already catches a thrown loss.
- **A timeline value signalled twice.** One real crash logged VUID-VkSubmitInfo-pSignalSemaphores-03242, signal value 807 on a timeline already at 807.
  It was on `pSignalSemaphores[1]`, which is a completion value.
  Whether it came before the loss or after is not known, so it may be a cause or a symptom.
  The same VUID against `UINT64_MAX` in another crash is only what a lost timeline reads.
