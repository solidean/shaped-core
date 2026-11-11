---
created: 2026-11-11
refs:
  - test: sg vulkan backend / vulkan / sg - timestamps from hundreds of lists in flight at once all read back
closed: 2026-11-11
resolution: fixed
---

# vulkan: copying timestamp results sometimes races a prior read of the same buffer (WRITE_AFTER_READ)

The test fails about one run in three on Windows, NVIDIA, with the validation layer's synchronization checks on (2026-11-11):

```
[sg.vulkan] validation: vkQueueSubmit(): pSubmits[0].pCommandBuffers[0] WRITE_AFTER_READ hazard detected.
vkCmdCopyQueryPoolResults ... No sufficient synchronization is present to ensure that a write (VK_ACCESS_2_TRANSFER_WRITE_BIT)
at VK_PIPELINE_STAGE_2_COPY_BIT does not conflict with a prior read (VK_ACCESS_2_TR...
Hint: An execution dependency is sufficient to prevent this hazard.
Buffer access region: offset = 0, size = 16
```

So a timestamp readback copies into a buffer region a previous submission still reads, without a barrier between them.
Hypothesis: a readback buffer is reused across lists in flight while an earlier copy or host read of it has not been made available yet.
Met while shipping unrelated nexus and clean-core changes; two of three reruns passed.

After merging e2980e9f4 (the first list of each epoch begins with a barrier) and ccbd29ada (a submission waits for the epoch before its own), the test passed 10 runs of 10 (2026-11-11).
