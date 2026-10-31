---
created: 2026-10-31
refs:
  - file: libs/graphics/shaped-rendering/tests/denoise-test.cc
  - test: "sr - dlss ray reconstruction denoises, and refuses a call without its guides"
---

# The DLSS rebuild test reads a process-wide release counter that parallel tests also move

`denoise-test.cc:1088` checks `sr::impl::dlss_released_stream_count() == released_before + 1` after an `idle_completion`.
The counter is process-wide, so any other test releasing a DLSS stream in the same window moves it too.

Measured 2026-10-31: the `debug-nopch` leg of `check` failed it with `3 == 2`, on the dx12 hardware adapter (RTX 5070 Ti).
Run alone in the same preset, the test passed.
The change under test then did not touch DLSS, only capture writes and the viewer's convergence rule.

Hypothesis, not verified: the shaped-viewer and shaped-rendering tests that run DLSS Ray Reconstruction overlap with this one in the shared phase.
Either the test goes `nx::config::exclusive("dlss")` with the others that release streams, or the counter becomes per context.
