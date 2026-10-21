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

Seen again on CI's Windows Clang WARP leg on 2026-10-21 (run 39599242298), the same 1 against 3, at a commit that touched only the issue tracker.

## Suspected cause, addressed in the test

The warm-up waited for `executed`, and a trace executes while a permutation still compiles behind a stand-in.
Every such frame restarts the mean, so frames 1 to 3 counted 1 whenever a compile outlasted the warm-up, which a busy suite and a slow device both make likely.
The warm-up now lasts until two frames accumulate.
It stays open until CI has run it under load for a while without a repeat.
