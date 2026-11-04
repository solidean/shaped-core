---
created: 2026-11-03
refs:
  - file: libs/graphics/shaped-viewer/tests/raster-without-raytracing-test.cc:132
  - test: "sv dx12 - a traced layer on a device without ray tracing is declined, and says so once"
---

# The traced-layer-declined test fails its 35 s wall-clock budget under check load

In a full `dev.py check` on 2026-11-03, on the `singlethreaded-clang` preset, the test ran 46 s and failed `cc::current_time_steady_secs() - start < 35.0`.
Its `nx::expect_error("on a device without sg::feature::raytracing_pipeline")` then saw no error, since the run gave up before the decline was logged.
Run alone on the same preset it passes in 1.5 s, so the budget is what the load of a full check exceeds, not the decline that is slow.

A budget measured in wall-clock seconds is a wait for a duration, which the test guidelines rule out: the wait should be for the decline itself.
The timing above is measured.
Which step of the test the 46 s went into is not known.
