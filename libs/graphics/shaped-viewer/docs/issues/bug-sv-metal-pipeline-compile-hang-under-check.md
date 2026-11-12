---
created: 2026-11-12
refs:
  - test: sv agreement - every kind of content draws the same in every mode
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_raster_pipeline.cc:305
---

# sv's agreement test hung once in Metal's render pipeline compile, in a ship's check

In `dev.py ship`'s check on 2026-11-12, on an Apple M4, shaped-viewer-test stopped for 60 s and the nexus watchdog ended it.
The test awaited was `sv agreement - every kind of content draws the same in every mode`.
The one stack doing work stood in `newRenderPipelineState` (`metal_raster_pipeline.cc:305`), called from a pipeline build on the async pool.
sg's context had 4 items outstanding; no scope was open on any thread.

Measured: run alone right after, the test passed three times in a row, 36 s a run; it passed in the check an hour earlier on the same code.
So it needs the check's load beside it.
Hypothesis: Metal's compiler stalls under contention, the way bug-sv-metal-device-lost-under-sanitized-check's timeout did; a deadlock inside it is the other reading.
The crash report and the recording of that run are in the build's `crashes/incoming/shaped-viewer-test-20261112-172934-97364*`; the next hang's stacks are what to compare.
