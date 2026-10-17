---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/tests/headless-viewer-test.cc
  - symbol: sv::viewer::finish_frame
---

# Nothing tests the pump sweep in `viewer::finish_frame`

**No test shape reached it.**
`(void)cc::thread_pump_all()` exists so an unthreaded viewer drives the copy actor on a frame whose epoch wait does not block.
Every test loop runs flat out, so the CPU is always `buffer_count` epochs ahead and the wait always blocks.
A blocking wait sweeps the registry itself, which makes the sweep redundant in every test and the line removable without a failure.

Four shapes were tried and all four passed with the line deleted:
the GPU-bound loop, a `refresh_rate`-throttled one, one that stops tracing after the first frame, and that one again with the stream throttled to 0.001 and 2048 triangles in flight.
What would reach it is a loop paced so the GPU is never behind — a real display's vsync, or a frame limiter — which no headless test has.
The two honest options are a test that paces on a condition rather than a duration, or an injected clock on the frame loop.
