---
created: 2026-10-17
tags: [container]
parent: epic-cc-container-api-gaps
refs:
  - file: libs/base/clean-core/src/clean-core/container/vector.hh
  - file: libs/base/clean-core/src/clean-core/container/unique_vector.hh
  - file: libs/base/clean-core/src/clean-core/container/array.hh
  - file: libs/base/clean-core/src/clean-core/container/unique_array.hh
---

# Retyping APIs on the allocating containers

`vector`, `unique_vector`, `array` and `unique_array` cannot yet hand their storage over as a container of another element type.
Only the TODO list in each header records the wish; no design exists yet.
