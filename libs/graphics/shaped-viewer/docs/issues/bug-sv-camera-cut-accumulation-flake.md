---
created: 2026-10-20
refs:
  - test: sv - a camera cut waits for a traced frame and drops the temporal history
  - file: libs/graphics/shaped-viewer/tests/view-accumulation-test.cc:572
---

# The camera-cut accumulation test once counted one frame where it waits for three

A full shaped-viewer-test run on 2026-10-20 failed `store.accumulated_frames(cut_id) == 3` with 1, and left one async item carrying the test's context.
It ran beside `sv - every builtin material type compiles as a hit group`.
The test then passed 10 runs in a row on its own, and the whole binary passed on the next run, so it has been seen once, under load.
The change under test was SGL constant folding, which alters shader text and nothing about frame pacing.
