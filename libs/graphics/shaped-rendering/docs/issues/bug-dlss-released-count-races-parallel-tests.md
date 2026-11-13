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

Seen again 2026-11-13 on a laptop RTX 5070 with DLSS 310.9.1 freshly fetched, now as `sr::impl::dlss_open_count(ctx) == 1` reading 2.
It failed every full `shaped-rendering-test` run, on the hardware and native-dxil dx12 adapters, and passed 10 of 10 runs alone.
It fails the same with the session's only shaped-rendering change reverted, so the count is moved by the suite's other DLSS tests, not by a change.

Cause of the open count, found 2026-11-13: `dlss_open_count` is the device's, and D3D12 hands every context on one adapter the same device.
So the dx12-hw and native-dxil contexts share one record, and the second context's open makes it 2; the test now compares the count across the reload instead.
The released-stream counter above is still process-wide.
