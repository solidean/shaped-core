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

## Log

**2026-11-26: a stack, and a way to raise the rate, on the Arc B580.**
clean-core's crash handler now reports a heap corruption through a vectored handler, since fail-fast skips the unhandled-exception filter.
An experiment let an Intel GPU restore mesh-shaded pipeline blobs, which `dx12_bind.cc` forbids.
Then `uv run dev.py test shaped-viewer-test --repeat 5` crashed about one run in nine, 3 to 14 s in.
The one whole stack found the corruption in the D3D12 debug layer, allocating as it formatted a message inside a ray-tracing pipeline's `CreateStateObject`.
That is where it was found, not where the heap was written.
Two cut-off reports were running `sv raster_bound - a device without ray tracing or binding arrays resolves automatic to raster_bound`, which creates and drops a dx12 context.
That test alone, or the raster_bound tests together, 30 runs each, did not crash.
With mesh blobs forbidden again, as shipped, about 11 runs of the whole binary passed.
Ruled out: `UnregisterMessageCallback` returning while a callback is in flight, which a standalone test showed it does not; it waits.
Not known: the writer, which page heap would name, and that needs an administrator (gflags).
Related: bug-sg-intel-dx12-mesh-draws-nothing-after-blob-restores, the same restores drawing nothing.
