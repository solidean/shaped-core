---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_sync.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - test: compute-coherence*
---

# Run MSL's two memory-ordering spellings on a Mac

Both were read off the MSL spec, and no Metal device has run them.

- **The device-scoped `atomic_thread_fence`** behind a barrier in an entry point holding a `@coherent` member (EMIT-150).
  sg's `compute-coherence-test.cc`, spread over many workgroups, is what shows it publishes across threadgroups.
- **`img.fence();`** behind a store to an image the entry point also reads (EMIT-151).
  It wants a read-after-write test of one texel.
