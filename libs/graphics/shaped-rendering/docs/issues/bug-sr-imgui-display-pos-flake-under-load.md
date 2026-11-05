---
created: 2026-11-05
refs:
  - test: sr::imgui_routine - a non-zero display pos shifts what lands on the target
---

# sr's imgui display-pos test reads an empty pixel once under parallel GPU load on an M4

In `dev.py ship`'s check on 2026-11-05, on macos-arm-llvm-relwithdebinfo, the metal run failed `pixel_at(centered, 125, 125) != byte(0)`: the pixel read zero.
shaped-rendering-test alone on the same build passed all 211 tests, so it needs the check's other GPU binaries running beside it.
Whether the draw was lost or the readback raced it is not known yet.
