---
created: 2026-11-14
priority: next
refs:
  - file: libs/graphics/shaped-viewer/tests/agreement_test_support.hh
  - file: libs/graphics/shaped-viewer/tests/backend-agreement-test.cc
  - file: libs/graphics/shaped-viewer/tests/content-agreement-test.cc
  - test: shaped-viewer-test
---

# shaped-viewer's GPU tests take minutes: a device per test, a full sync and three readbacks every frame, no narrowing

## The rule (the user, 2026-11-14)

**A normal test run creates at most one device per backend type: one dx12, one vulkan, one metal, one webgpu.**
A test that needs a device takes the run's through an invocable; a test never creates a context of its own.
A device with something the shared one lacks, such as validation layers or a WARP adapter, belongs under `--thorough`, or in the one shared device's own configuration.
This needs a refactoring of the testing: the shared devices, how a test asks for a backend, and where each backend's configuration is decided.

## Measured, 2026-11-14 (RTX 4090, dx12 and vulkan)

shaped-viewer-test's 584 tests took 442 s in a full `ship` gate, 340 s alone, against 56 s for its 532 tests before five tests landed on Nov 3 and 4.
The slowest in the gate, each listed with what it does per run:

| Test | Gate | What it costs |
|---|---|---|
| sv vulkan - every mode and every denoiser draws the view it draws on dx12 | 133 s | creates its own vulkan device with validation layers, and a second dx12 device; four modes compile cold on both |
| sv agreement - every kind of content draws the same in every mode | 78 s (20 s alone) | five convergences to 512 or 1024 samples per pixel, at 64 by 64 |
| sv invariant - the scene mirrored is the image mirrored | 37 s | convergences through the same helper |
| sv agreement - every material type draws the same sphere in every mode | 34 s | the same |
| sv furnace - a sphere in a uniform environment reflects its albedo at every angle, in every mode | 25 s (10 s alone) | the same |

The gate's GPU contention made each 1.3 to 2.4 times slower than alone.

## Why each test is slow

- **A full sync and three readbacks every frame.** `agreement::converge` (agreement_test_support.hh) ends every frame with `ctx.idle_completion()`.
  It also downloads the mean, the normal guide and the albedo guide every frame, whether it reads them or not.
  At 64 by 64 the GPU's work is trivial, so the round trip is the cost.
  Measured on the content test alone: downloading only at the end of each half, and pacing the other frames at two epochs in flight, took it from 20.1 s to 14.7 s.
  The megakernel reference went from about 3 s to 1.3 s, the wavefront from about 5.8 s to 2.6 s.
  Every frame's finite-pixel check (`check_finite`) is deliberate; a check every Nth frame keeps most of it at a fraction of the cost.
- **A device per test.** The vulkan agreement test creates two, against the rule above, so every pipeline of four modes compiles cold twice, and validation slows every vulkan call.
- **No narrowing.** None of the five reads `nx::is_thorough()`; each runs its full sample counts and modes in every default run and every gate.
- **First-use compiles.** Each mode's pipelines compile in the first test that uses them: 0.3 to 2.9 s per mode here, paid once per device, which the rule above makes once per run.
- **No bound in the gate.** No timeout stopped the binary at 7.4 minutes; a GPU suite's binary needs an upper bound, so a regression fails rather than runs long.

## Done when

- Every normal test takes its device from the run's one per backend type, and none creates one.
- `agreement::converge` neither syncs fully nor reads back every frame.
- The expensive tests narrow their default run under `nx::is_thorough()`, and the full sweep runs there.
- shaped-viewer-test stays well inside its budget alone and in a full gate, with that budget enforced.
