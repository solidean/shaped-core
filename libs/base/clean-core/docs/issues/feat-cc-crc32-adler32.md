---
created: 2026-10-17
tags: [bytes]
refs:
  - file: libs/base/clean-core/src/clean-core/bytes/impl/deflate_backend.cc
---

# Expose CRC32 and Adler32

Wanted by every container format that stores a per-member checksum — zip, gzip, PNG — so this lands with the first of those rather than before it.
Neither is a hash in the `common/hash.hh` sense and neither is cryptographic; they belong next to the codecs, not next to `blake3`.

Both now exist in the tree, vendored with zlib and reached only by `deflate_backend.cc`.
Exposing them is a matter of deciding the API, not of finding an implementation.
