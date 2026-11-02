---
created: 2026-11-02
refs:
  - test: shaped-viewer-test
  - file: libs/data/blob-cache/src/blob-cache/default_cache.hh
---

# shaped-viewer-test dies of heap corruption at startup when the real shader cache is open

Measured on Windows, release-clang, a dx12 hardware adapter, at 6502ab017:

- `shaped-viewer-test` exits with 0xC0000374, heap corruption, about 3 s in, before any test reports; its output is the seed line alone.
- It is intermittent with the default cache open: of four runs, three crashed and one passed, directly before a crash.
- With typed-geometry put back to 6b7c7cc9d, which shaped-viewer does not reach either way, two runs passed; two runs decide nothing.
- With `SC_BLOB_CACHE=off` it passed four runs of four, typed-geometry at HEAD.
- It first showed in the pre-push gate right after the merge of 60de2cdb4 and 05d6550f6, which change what a compiled shader carries and how the codec stores it.
  The gate before that merge passed the same binary in release.

Hypothesis, not checked: an entry the real cache holds, written before or by a differently built binary, is decoded past its end by the new codec.
bug-bcache-tests-open-real-default-cache is why the test reads that cache at all.
A crash stack, or a run against a fresh empty cache, would decide it.
