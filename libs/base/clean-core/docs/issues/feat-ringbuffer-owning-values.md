---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/container/impl/ringbuffer_container.hh
---

# values() on a temporary ringbuffer

Every other container's `values() &&` moves the container into an owning source, so `make_ring().values()` would be the natural spelling.
A ringbuffer's `values() &&` is deleted instead, a deliberate limitation rather than an oversight.

Its elements wrap around the slot array, so an owning source cannot address them as one contiguous run.
The cheap fix is a `linearize()` in the owning source's constructor, which moves elements nobody asked to move.
The honest one is an owning source that keeps the two-segment chain, re-deriving both segments from the owned ring after each move, since a `fixed_ringbuffer` keeps its slots inline.

Revisit when a caller actually wants it.
