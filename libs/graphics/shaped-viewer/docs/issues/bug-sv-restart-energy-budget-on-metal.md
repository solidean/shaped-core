---
created: 2026-11-02
tags: [metal]
refs:
  - file: libs/graphics/shaped-viewer/tests/restart-energy-test.cc:146
  - test: "sv - restarting the accumulation keeps the image*"
---

# The restart-energy test runs out of its 60 s budget on an M4

On an M4 under Metal API validation, the third view of the test reaches about 80 of the 128 drawn frames its convergence loop wants before the 60 s REQUIRE fires.
It failed that way on four runs in a row on 2026-11-02, at main's 1c19503ff as well as with later changes, after passing in two full sv runs earlier the same afternoon.
The frames are drawn and accumulate (4 samples each), so it is speed rather than a stalled view.
The loop's budget also covers shader compiles, so a cold metallib cache costs it more.
Either the frame count or the budget wants tuning per backend, or the test wants to measure frames rather than wall time.
