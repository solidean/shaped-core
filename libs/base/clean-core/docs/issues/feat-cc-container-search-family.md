---
created: 2026-10-17
tags: [container]
parent: epic-cc-container-api-gaps
refs:
  - file: libs/base/clean-core/src/clean-core/container/vector.hh
  - file: libs/base/clean-core/src/clean-core/container/unique_vector.hh
  - file: libs/base/clean-core/src/clean-core/container/ringbuffer.hh
  - file: libs/base/clean-core/src/clean-core/container/fixed_ringbuffer.hh
---

# `contains` / `find` / `count` and their `_where` forms on the containers

`vector` and `unique_vector` lack `contains`, `contains_where`, `find`, `find_where`, `count` and `count_where`.
`ringbuffer` and `fixed_ringbuffer` lack the same family, plus the predicate-based removal family the allocating containers already have (`remove_all_where` and its siblings).
