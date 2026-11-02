---
created: 2026-09-21
tags: [vulkan, flaky]
refs:
  - test: shaped-graphics-test
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_upload_async.hh
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_download_async.hh
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_epoch.cc
  - symbol: sg::impl::transfer_scheduler
---

# Synchronization validation reported a `WRITE_AFTER_WRITE` between two transfer copies

Once in about three loaded runs.
All 479 tests passed; the run failed on eight unattributed checks from the validation listener, each naming one transfer command buffer writing one `VkBuffer` twice.

Two copies in one async window cannot happen: each window records one copy into its own of the three reused command buffers, so the same handle is a slot reused three windows later.
Per-destination order holds, since `transfer_scheduler` only picks a family's head, and every window carries a memory barrier ahead of its copy.

**The likely cause is the layer rather than the copies** (hypothesis).
A window submit waits on other timelines, and the layer defers checking a submission whose waited value it thinks is unreached.
It replays that check inside a later submit, which is the path `vulkan_epoch.cc` already avoids for host signals.

**What is in place for the next occurrence:** both async transfer systems keep their last 32 windows.
The sg vulkan driver appends to an `_AFTER_WRITE` failure the ones whose command buffer, destination or staging buffer the message names.

It was seen again on 2026-09-21 under the full suite, as `WRITE_AFTER_READ` and `WRITE_AFTER_WRITE` on two buffers the stream test did not own.
That dump was useless: it printed every upload window, all to one other buffer, and the log's size cap cut it off before the download windows.
A system none of whose windows match now says so, which rules its copies out: the next dump either shows the two copies or clears both systems.

If the two copies overlap and were queued out of submission order it is a real bug; if they are disjoint or in order it is the layer.
The experiment beside it is dropping `wait_token` and `download_wait` from a window submit once the host counter shows them reached, which the comment at that wait chose not to do.
