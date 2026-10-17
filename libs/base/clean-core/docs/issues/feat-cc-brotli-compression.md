---
created: 2026-10-17
priority: someday
tags: [bytes]
refs:
  - file: libs/base/clean-core/src/clean-core/bytes/compression.hh
  - symbol: cc::compression_algorithm
---

# Further compression algorithms: brotli next, if a format demands it

Deflate landed over vendored zlib — `compression_algorithm::deflate`, with `frame` meaning gzip and a third `compression_framing::zlib` for the RFC 1950 wrapper.

Nothing else is queued.
Brotli would be the next interop codec if some format demanded it, and would slot in the same way.
