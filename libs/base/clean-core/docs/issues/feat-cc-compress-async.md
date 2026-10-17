---
created: 2026-10-17
tags: [bytes, async]
refs:
  - file: libs/base/clean-core/src/clean-core/bytes/compression.hh
  - file: libs/base/clean-core/src/clean-core/algorithm/sort_async.hh
---

# `compress_async`

Mirroring `algorithm/sort_async.hh`: explicit extra API over the synchronous core, never a hidden worker pool.
zstd's own multithreading is deliberately not enabled, so this would chunk over `cc::async` instead and keep the memory cost where the caller can see it.
