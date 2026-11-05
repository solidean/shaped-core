---
created: 2026-11-05
refs:
  - test: "sg vulkan - a timestamp pair measures real work"
  - file: libs/graphics/shaped-graphics/backends/vulkan/tests/vulkan-query-test.cc
closed: 2026-11-05
resolution: fixed
---

# A vulkan timestamp pair reads nothing on an AMD iGPU

On the AMD Radeon iGPU of a Windows machine (an Intel Arc B580 beside it, run with `VK_LOADER_DRIVERS_SELECT=*amd*`), the test fails.
It read `elapsed == -42.94967297` seconds once, and exactly 0 on every later run.

## Cause (2026-11-05)

- **The ticks were right in the pool and lost on the way out.**
  `vkGetQueryPoolResults` after the epoch retired read two real ticks 2.1 ms apart, while sg's readback of the same pair read `0 -> 0`.
- sg copies the results into a buffer with `vkCmdCopyQueryPoolResults` and reads that buffer back, behind a copy-to-copy barrier from its tracker.
  Synchronization2 puts that command in the copy stage, so the barrier is enough by the letter; AMD's Windows driver writes it from another stage, and the readback copied what the buffer held before.
- **Fixed** by a barrier from every stage after the query copy, once per list that records a timestamp.
  The test then passes twenty runs in a row on that iGPU, and the vulkan backend suite on it and on the Arc.
- The wrapped-counter hypothesis was wrong: the iGPU's graphics family reports 64 valid bits.
  So were a missing dependency before the copy and the host-side pool reset, which were each tried and changed nothing.
