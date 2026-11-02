---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/memory/node_allocation.hh
  - file: libs/base/clean-core/src/clean-core/memory/node_allocation.cc
  - file: libs/base/clean-core/docs/systems/node-allocation.md
  - symbol: cc::node_allocator::allocate_node_bytes
---

# A build mode where every node is a real allocation, so a leaked node is visible to a leak checker

A small-class node comes out of a 64-slot slab, so a leaked one is unused bytes inside a slab that stays reachable — LeakSanitizer has nothing to report, in every configuration.
`SC_MIMALLOC=OFF` made the *slabs* visible; nodes sit one level below where it reaches.
It is not free either: `node_trim_ring` reclaims only a *fully* free slab, so one leaked 32-byte node pins its whole 2 KB slab for the life of the process.

The mechanism already exists as the large-node path.
`node_allocator::allocate_node_bytes` branches at `idx > small_max` to `allocate_node_bytes_large`, one `cc::default_memory_resource` allocation per node.
`node_allocation_free_large` mirrors it on the way out.

So the mode is that branch always taken, under a `CC_NODE_ALLOC_DIRECT` derived from a CMake input the way `CC_HAS_MIMALLOC` is.
Compile-time rather than a runtime flag, because the branch sits in a `CC_FORCE_INLINE` function on the hottest path clean-core has.
Set it in the `sanitize-*` presets, beside `SC_MIMALLOC=OFF`.

Verify first that nothing outside the slab paths recovers a slab base from a node pointer.
`ptr & ~node_slab_mask_for_class(idx)` holds for a slot and not for a directly-allocated node.

The cost to state when it lands: `align_up(node_large_header_size, alignment)` turns a 1-byte class-0 node into a 24-byte allocation.
So the mode is a memory multiplier as well as a speed one.
