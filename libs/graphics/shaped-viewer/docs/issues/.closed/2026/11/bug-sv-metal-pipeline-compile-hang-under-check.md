---
created: 2026-11-12
refs:
  - test: sv agreement - every kind of content draws the same in every mode
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_raster_pipeline.cc:305
closed: 2026-11-13
resolution: fixed
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

## Log

2026-11-13: not a hang and not a deadlock in Metal's compiler; it was one slow compile in a long test, under a watchdog too short for it.
macOS's log has the build beginning at 17:29:18 and succeeding at 17:29:34.013 after 15.7 s, the second the watchdog reported; sv drew frames until 5 ms before the report.
The build was the hybrid fragment stage, which takes Metal some 15 s per variant from a cold cache (perf-sv-hybrid-metal-compile-cost).
The run's first SGL change since the last warm run had left Metal's cache cold, which is why the test passed alone right after.
Reproduced alone with the cache set aside: the hybrid mode added no samples for 141 s, past the converge loop's frame budget, which counted declined frames.
Two fixes:
`converge` now bounds frames that add no samples by time, 240 s a stretch, and counts only frames that add samples against its budget; the cold run passes.
`dev.py check` gives each test binary a watchdog of half its own timeout, as `dev.py test` does, rather than nexus's 60 s default, which ignored sv's declared 600 s.
