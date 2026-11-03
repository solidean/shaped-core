---
created: 2026-11-03
priority: next
refs:
  - file: libs/base/clean-core/src/clean-core/memory/node_allocation.cc
closed: 2026-11-03
resolution: fixed
---

# Node allocation walks the whole slab ring on every exhaustion, so allocating many live nodes is quadratic

`allocate_node_bytes_non_fast` runs when the head slab's local freemap is empty, every 64 allocations of a size class in a bulk phase.
It walks the class's whole slab ring from the head, an atomic exchange on each slab's remote bitmap, before it refills.
In a bulk phase every slab but the newest is full, so each exhaustion costs the number of live slabs, and n allocations cost n^2 / 4096 slab visits.
The code says so: "Still O(ring) per exhaustion for alloc-heavy workloads".

Measured on a Zen 5, release, in instructions retired and L1D read misses, allocating one-limb `tg::bigint`s into a vector:
| live nodes | instructions per allocation | L1D misses |
|---|---|---|
| 11,747 | 240 | 220,000 |
| 114,000 | 1,470 | 19,300,000 |

It decides a polynomial product with 114,000 output terms: 148 M instructions, most of them here, and an IPC of 0.7.

Fix options, to be decided:
- Track the slabs with free slots: an owner's free that takes a slab's local freemap from 0 to non-zero pushes it onto a per-class stack,
  and an exhausted head pops from it before walking.
- Remote frees can do the same through a lock-free per-class stack, or keep the walk but bound it and run it only every so many refills.
- The trim's walk shares the cost model and should follow.

## Resolved: a bounded walk, a clock hand over the ring

An exhaustion visits at most 8 slabs, resuming where the last walk stopped; a walk that finds nothing refills at the hand.
A remote bitmap is read with a plain load before its exchange, so a full slab costs no locked instruction.
The price is documented in docs/systems/node-allocation.md: a slot freed behind the hand waits a lap, and the ring grows up to 12.5% meanwhile.

Measured on the 9950X3D2, release, the sparse 114,000-term product in labs/bignum's polynomial counters:
| | instructions | cycles | L1D misses |
|---|---|---|---|
| full lap per exhaustion | 147.2 M | 184.8 M | 10.35 M |
| at most 8 slabs | 94.5 M | 41.3 M | 0.099 M |

Exact reuse, the owner's frees listing a full slab again, stays a follow-up in the same doc.
