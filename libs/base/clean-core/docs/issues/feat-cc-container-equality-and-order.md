---
created: 2026-10-17
tags: [container]
parent: epic-cc-container-api-gaps
refs:
  - file: libs/base/clean-core/src/clean-core/container/vector.hh
  - file: libs/base/clean-core/src/clean-core/container/unique_vector.hh
  - file: libs/base/clean-core/src/clean-core/container/array.hh
  - file: libs/base/clean-core/src/clean-core/container/unique_array.hh
  - file: libs/base/clean-core/src/clean-core/container/fixed_array.hh
  - file: libs/base/clean-core/src/clean-core/container/ringbuffer.hh
  - file: libs/base/clean-core/src/clean-core/container/fixed_ringbuffer.hh
---

# Equality and ordering for the sequence containers

`vector`, `unique_vector`, `array`, `unique_array`, `fixed_array`, `ringbuffer` and `fixed_ringbuffer` have no `operator==` and no `operator<=>`.
Hashing has landed on all of them, as a `hash` hidden friend over `cc::make_hash_range`, so equality is the half that is missing for use as a key.
