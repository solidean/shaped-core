---
created: 2026-11-05
refs:
  - test: "sg vulkan - a timestamp pair measures real work"
  - file: libs/graphics/shaped-graphics/backends/vulkan/tests/vulkan-query-test.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-05T15:33Z
---

# A vulkan timestamp pair reads a negative delta of exactly -2^32 ticks on an AMD iGPU

On the AMD Radeon iGPU of a Windows machine (an Intel Arc B580 beside it, run with `VK_LOADER_DRIVERS_SELECT=*amd*`), the test reads `elapsed == -42.94967297` ms.
That is −2³² ticks at the device's 10 ns period, so the second timestamp wrapped past a 32-bit boundary the first did not.

## Hypothesis, unconfirmed

- The queue family reports `timestampValidBits` of 32 and the delta is not taken modulo 2^validBits, or the read masks one value and not the other.
