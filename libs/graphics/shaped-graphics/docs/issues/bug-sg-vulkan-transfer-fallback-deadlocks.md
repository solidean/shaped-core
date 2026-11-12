---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_context.create.cc
  - test: sg stream - promote_to_async makes a later list wait on the transfer
---

# Async transfer on the graphics queue, the fallback for a device with no spare queue, hangs promote_to_async

What is wrong or wanted, and what is already known.

`vulkan_context.create.cc` gives async transfer the graphics queue when the device can spare no queue for it, and says that stays correct.
On 2026-11-07 that path was forced on the Arc B580, by setting both transfer queues to `VK_NULL_HANDLE` before `set_transfer_queues`.
It was a measurement for [bug-sg-vulkan-device-lost-under-repeat](bug-sg-vulkan-device-lost-under-repeat.md).
The `vulkan` context then hung on its first iteration until the watchdog, in `sg stream - promote_to_async makes a later list wait on the transfer`.

That test submits a list which waits on a stream's completion value, and on one queue the copy that signals it is queued behind that list.
A wait before its signal in one queue's FIFO is a deadlock rather than a slow path, which is likely what this is; it was not read further.
No device here lacks a spare queue, so nothing has met it outside this experiment.
