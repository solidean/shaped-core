---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.hh
  - symbol: sv::viewer
---

# The frame API blocks, so the viewer cannot run on a never-block context

`sv::viewer` is a synchronous pull loop — `is_running()`, `end_frame()`, a draining destructor.
So it throttles and drains with `cc::async_blocking_get` on sg's completions, behind a `may_block` assert.
sg has no blocking spellings left, so this is the one place the viewer cannot run on a never-block context such as WebGPU.

The shape that retires it is cube-editor's: `end_frame()` returning an async that awaits `epochs_in_flight_completion`, and `finish()` in place of the destructor's drain.
