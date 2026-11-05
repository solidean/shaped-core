---
created: 2026-11-03
priority: next
refs:
  - file: libs/base/clean-core/src/clean-core/memory/node_allocation.cc
closed: 2026-11-03
resolution: fixed
---

# A late owner free into an orphaned slab writes local while another thread may adopt it

`system_reclaim_slabs` orphans a slab that still holds live nodes with its owner stamp unchanged, the exiting thread's token.
A `thread_local` constructed before the thread's allocator is destroyed after it, and a node it frees then takes the owner path: a plain write to `local`.
Meanwhile another thread may pop the same slab from the orphan bin, restamp its owner and read `local`: a data race, and a lost or doubled slot.
The bin's lock orders nothing here, because the late free takes no lock.

Found by reading the code; not reproduced under a race detector.
The fix: stamp an orphan with a reserved owner token no thread holds, so a late free routes to `remote`, which the adopter drains.

## Resolved

Reclaim stamps every orphan `node_orphan_owner`, `~0`, which no thread's token equals with or without the full bit.
The test "a free after its thread's allocator is gone routes to remote" frees from a `thread_local` destroyed after the allocator: it fails without the stamp, with the late free in `local`.
