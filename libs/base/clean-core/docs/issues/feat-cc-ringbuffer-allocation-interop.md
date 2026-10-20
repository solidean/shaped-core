---
created: 2026-10-17
tags: [container]
parent: epic-cc-container-api-gaps
refs:
  - file: libs/base/clean-core/src/clean-core/container/ringbuffer.hh
  - symbol: cc::ringbuffer
---

# `ringbuffer` adoption and extraction of its allocation

It holds its `cc::allocation<T>` as a raw byte handle, so neither `create_from_allocation` nor `extract_allocation` exists yet.
Adoption can take the largest power of two at or below the incoming capacity; extraction needs the content linearized first, or a check that it already is.
