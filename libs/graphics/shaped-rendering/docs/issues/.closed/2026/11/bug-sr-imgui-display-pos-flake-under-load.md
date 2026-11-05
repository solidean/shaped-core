---
created: 2026-11-05
refs:
  - test: sr::imgui_routine - a non-zero display pos shifts what lands on the target
closed: 2026-11-05
resolution: fixed
---

# sr's imgui display-pos test reads an empty pixel once under parallel GPU load on an M4

In `dev.py ship`'s check on 2026-11-05, on macos-arm-llvm-relwithdebinfo, the metal run failed `pixel_at(centered, 125, 125) != byte(0)`: the pixel read zero.
shaped-rendering-test alone on the same build passed all 211 tests, so it needs the check's other GPU binaries running beside it.
Whether the draw was lost or the readback raced it is not known yet.

**Not load: the newest residency-set additions were missing from a fresh context's first work.**
`sr::imgui_routine - a shader reload keeps drawing` failed alone, every time, in a fresh process.
`sr - blit into a bgra8 target keeps each channel` failed the same way, and both read back zeros 20 ms in.
It bisected to the commit that removed a zeroed buffer the metal context made at creation: one more residency addition after the rings had hidden it.
An unused allocation or a second empty commit did not help, and a new addition did.
`requestResidency` after every addition fixed it: 30 of 30 runs of shaped-rendering-test, and 5 of 5 rounds of the GPU binaries together.
Why a committed addition can be absent is a hypothesis; the reproducer is the reload test run alone, with `--repeat 10`.
