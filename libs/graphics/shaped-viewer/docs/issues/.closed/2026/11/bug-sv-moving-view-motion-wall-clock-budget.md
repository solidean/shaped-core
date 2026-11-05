---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-viewer/tests/moving-view-motion-test.cc
  - test: "sv - a view stepping sideways in front of a wall reports the step as every pixel's motion"
closed: 2026-11-05
resolution: fixed
---

# The moving-view motion test fails on a loaded machine by its 60-second wall-clock budget

What is wrong or wanted, and what is already known.

The test's loop requires `current_time_steady_secs() - start < 60.0` on every iteration, so it fails when the machine is slow rather than when the view's motion is wrong.
Seen on 2026-11-05 on dx12 hardware during a full suite in which shaped-viewer-test took 207 s instead of its usual 60 to 140: 60.0016 s against the 60 s budget.
Load is the normal state on a dev box, so the bound wants to count frames or iterations, as a condition the test waits for, rather than seconds.
