---
created: 2026-10-17
tags: [vulkan, flaky]
refs:
  - file: libs/graphics/shaped-graphics/tests/transfer/overlapping-readback-test.cc
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_download_inline.hh
  - test: a rendering scope clears, draws and stores
---

# A texture readback can come back all zeroes when two lists record concurrently on one context

**Probably fixed; re-test it against the entry-barrier model before treating it as open.**

Seen only on vulkan, on Windows, while the tier-2 suite briefly shared one context.
About one run in eleven, `vulkan-raster-test.cc`'s "a rendering scope clears, draws and stores" read its target back as 4096 zero bytes.
Not another test's pixels and not a wrong colour — nothing from either render scope survived, and the clear alone would have left alpha at 255.
No validation message accompanied it.
Run alone the test passes 30/30, so it needs the concurrency.

**The ring-ordering hypothesis is excluded.**
`cmd.download`'s staging is reserved while a list records, and its jobs are enqueued when a list submits.
`vulkan_download_inline.hh` claims those orders are the same, and they are not once two lists are open.
But each job carries its own `deferred_cpu_copy` closure over its own reservation, so drain order decides when a copy runs and never which window it reads.
[overlapping-readback-test.cc](../../tests/transfer/overlapping-readback-test.cc) reserves in one order and submits in the other deliberately, and passes 20/20 on every backend.
It stays as the gate for that.

The tier-2 suite is back to a context per test, so nothing we ship reaches this today — which also means it cannot be reproduced on demand any more.

**The likely cause has since been found and fixed**, though not by chasing this.
Two lists recorded concurrently against one resource each computed their barriers against the state it was in *while they recorded*.
So neither saw the other's declares, and both took the no-barrier freebie.
That is exactly this shape — a readback that needs the concurrency and sees nothing.
