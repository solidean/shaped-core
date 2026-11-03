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
A GPU timeout under contention is the likely reading, not confirmed; which command buffer failed is not known yet.
