---
created: 2026-10-29
refs:
  - test: sr - metalfx_rr denoises and upscales a noisy stream through the front
  - file: libs/graphics/shaped-rendering/tests/backends/metal-entry.cc
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
