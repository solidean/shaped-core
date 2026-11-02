---
created: 2026-10-17
tags: [vulkan, flaky]
refs:
  - test: shaped-graphics-vulkan-test
---

# A shared async pool can outlive the device a node's value belongs to

Seen once, under a full `check` (five presets building and testing at once).
`vkDestroyDevice` reported two `VkPipeline`s and their `VkPipelineCache`s leaked, from a tier-2 test that had already dropped every handle to them.

The pipeline cache releases its providers at shutdown, so the only remaining owner is the `cc::async` node the build ran on.
And the pool is process-wide while a device is per test.

Not reproduced in isolation: 40 repeats of that test, three full-suite runs and a second `check` are all green, so it needs the contention.
The single-threaded pool had the same shape and was fixed by dropping finished nodes in `participate_until_ready`.
Whether the threaded one retains a finished node anywhere is the thing to establish.
