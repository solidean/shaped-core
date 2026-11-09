---
created: 2026-10-29
refs:
  - test: sr - metalfx_rr denoises and upscales a noisy stream through the front
  - file: libs/graphics/shaped-rendering/tests/backends/metal-entry.cc
closed: 2026-11-09
resolution: fixed
---

# sr's metal tests overflow metal's upload ring under parallel GPU load

Once in a `dev.py check` on an M4, the macos-arm-llvm-release run of this test logged thirteen undeclared warnings and failed.
Each was sg.metal's "an inline upload of 16384 bytes did not fit the 16777216-byte upload ring", once per epoch.
A 16 KiB upload not fitting a 16 MiB ring means earlier epochs still held it, which the check's parallel test binaries sharing one GPU make likely.

It did not reproduce: twenty repeats of shaped-rendering-test alone and four of the whole release suite in parallel all passed (run seed of the failure: 3040842520677762577).
Either the test declares the warning as possible under load, or something holds the ring longer than an epoch should.

On 2026-11-03 it hit seven tests in one check, on two presets, once sr's SGL routine tests ran on metal too: svgf, atrous, BC compression, metalfx and slug.
shaped-rendering-test alone passed five repeats.
sr's metal driver now allows the warning with `NX_ALLOW_LOGS`, so a check no longer fails on it.
What stays open is the second question above: whether anything holds the ring longer than an epoch should.

## Resolution

Measured on 2026-11-09: nothing held the ring past its epoch's retirement; nothing *retired* it.
An epoch's ring bytes are freed only by a retire sweep, and `advance_epoch` runs none.
300 plain advances with a 64 KiB upload each freed none of their 300 checkpoints while the GPU had finished 234 of them.
sr's suite alone, on an idle M4, peaked at 16,775,600 of the ring's 16,777,216 bytes and overflowed twice.

Two layers advanced without sweeping: each test's frame loop, and the backend driver running every test in sequence on one shared context.
The frame loops now await `epochs_in_flight_completion(2)` before each advance.
`nx::invocation_options::after_each` lets a sequential driver await something after each child, and the sg, sr and sv drivers pass `sweep_after_each`, which sweeps once a test submitted.
The suite now peaks at 8.3 MB, all of it one test's own epoch, with no overflow; the allowed warning is gone.
`sg metal - advancing reclaims no staging, and a frame loop bounded at two in flight stays inside the ring` pins both halves, and sg's epochs doc says it.
