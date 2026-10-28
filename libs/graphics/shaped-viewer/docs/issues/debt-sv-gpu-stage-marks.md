---
created: 2026-10-28
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/gpu_marks.hh
---

# sv's GPU stage marks stand in for GPU scopes sg should record into cc::rec

`sv::impl::gpu_mark` records a named GPU timestamp at a stage boundary of a frame, while a caller has set the thread's collector.
`hybrid-benchmark.cc` sets one to break the hybrid's whole frame down by stage, since its total was far above the hybrid's own cost.

**It is a workaround**: a thread-local hook in one library, read by one benchmark.
The real piece is a GPU counterpart of `CC_RECORD_SCOPE` in sg: a scope recording a timestamp pair into the command list, resolved into `cc::rec` once the list completes.
Then every routine's passes would show in the same recording as their CPU scopes, and the marks here would go.
