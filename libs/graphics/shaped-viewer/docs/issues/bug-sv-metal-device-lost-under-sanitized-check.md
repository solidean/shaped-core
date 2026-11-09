---
created: 2026-11-03
tags: [metal]
refs:
  - file: libs/graphics/shaped-viewer/tests/metal-entry.cc:30
---

# sv's metal tests lose the device once in a sanitized check on an M4

In `dev.py ship`'s check on 2026-11-03, the sanitize-macos-arm-llvm run of shaped-viewer-test lost the metal device 40 s in.
The loss was a failed command buffer, MTL4CommandQueueErrorDomain error 1, and every sv test after it failed with it (run seed 8230675655398155205).
Run alone on the same build, the whole binary passed repeatedly at about 277 s a run, so it needs the check's other GPU binaries running beside it.
A GPU timeout under contention is one reading; a page fault is the other, since MTL4 reports one as the same timeout code.

On 2026-11-09 a failed commit's error was made to name its GPU time and what its list held: its submission, draws, dispatches, ray dispatches and builds.
A hang runs to the watchdog, seconds, and a fault ends early, so the next occurrence's message tells the two apart and names the work.
The full sanitize-macos-arm-llvm suite passed the same day on an M4, so it was not reproduced; the next failure's message is what to read.
