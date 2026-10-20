---
created: 2026-10-17
parent: epic-sg-transfer-gaps
tags: [vulkan]
refs:
  - symbol: sg::context::async_ready_layout
  - symbol: sg::async_direction
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/barrier/resource_access.hh
  - test: sg stream - a texture sink receives whole tightly-packed rows
---

# A direction-specific async-ready layout

Postponed for simplicity, not blocked.
`async_ready_layout` returns `general` on every backend, so `sg::async_direction` is accepted and ignored — a caller's statement of intent rather than an answer.
Vulkan could copy from `TRANSFER_SRC_OPTIMAL` and into `TRANSFER_DST_OPTIMAL` and keep whatever compression that buys.
dx12 could not: its copy queue needs COMMON either way.

**One layout everywhere is what needs no implementation.**
It is slower in general, and it is the reason the direction stays in the API: the shape extends easily and would be hard to add back.

**What the naive attempt runs into is submit-call order.**
The fixup that settles a texture's layout runs at *enqueue*, on the calling thread, while a transfer already enqueued has not necessarily been submitted by its actor yet.
An upload followed by a download of one texture therefore puts the download's fixup ahead of the upload's copy in call order.
The validation layer tracks image layouts in `vkQueueSubmit` call order and models no semaphore, so it reads the upload's copy as naming a layout the image has left.
The GPU ordering is correct throughout; only the layer disagrees, and a layer message fails a test.
One layout for both directions removes the second fixup, and with it the interleave.

The reproduction is [stream-test.cc](../../tests/transfer/stream-test.cc)'s `a texture sink receives whole tightly-packed rows`.
It failed about one run in ten with direction-specific layouts, and passes 40/40 with one.

**What earns it back is ordering the fixups rather than avoiding them.**
Submission and the async / stream entry points are all serialized against each other already.
An upload and a download of one resource cannot overlap either, since each waits on the other.
So the transitions a transfer needs can be inserted on the direct queue in that same order, which makes call order match queue order and the interleave impossible.
That is a real piece of work: a direct-queue submit placed against each job rather than at enqueue.
So it is a quality-of-implementation follow-up rather than part of the change that found it.
[bug-sg-dx12-list-strands-transfer-layout](bug-sg-dx12-list-strands-transfer-layout.md) wants the same change.
