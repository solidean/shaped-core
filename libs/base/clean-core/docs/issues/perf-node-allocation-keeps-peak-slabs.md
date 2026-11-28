---
created: 2026-11-28
refs:
  - file: libs/base/clean-core/src/clean-core/memory/node_allocation.cc
---

# Node allocation: a thread keeps the slabs of its peak once its ring passes 64

`node_trim_ring` gathers a class's ring into a buffer of 64 slabs and leaves a longer ring alone, as [node-allocation.md](../systems/node-allocation.md) says.
So every slab a thread's peak needed stays with that thread after its nodes are freed, empty, for as long as the thread lives.

Measured: a workload on a 15-worker `async_thread_pool` that allocates bursts of 32-byte nodes on the workers and frees most of them on other threads.
Once it is done and every node is freed, each worker still holds some 2,700 empty 2 KiB slabs of that class, and a worker that once carried a big burst some 22,000, 44 MB.
Repeating the workload adds no slab to a worker already at its peak, but another worker reaches it, so the retained total grows toward workers times peak: some 670 MB here.
Read by walking mimalloc's live blocks: every such slab's local and remote freemaps were full, so no live node pointed into it.

Wanted: a long-lived thread gives back what its peak left once that peak is over.
Two shapes, not measured yet:
- trim when a thread goes idle, which `async_thread_pool` workers and parked threads already mark by publishing their allocator counts, with an in-place walk of any length there;
- an in-place, uncapped walk on the cold path, amortized against the ring's length rather than run every `node_trim_period` exhaustions.
