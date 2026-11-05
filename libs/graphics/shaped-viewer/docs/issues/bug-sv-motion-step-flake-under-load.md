---
created: 2026-11-05
refs:
  - test: "sv - a view stepping sideways in front of a wall reports the step as every pixel's motion"
---

# sv's sideways-step motion test reports twice the expected motion once under parallel GPU load on an M4

In `dev.py ship`'s check on 2026-11-05, on macos-arm-llvm-relwithdebinfo, the metal megakernel run failed with `worst < 0.02` at 0.2217.
The expected motion was 0.1109 at centre (0.333, 0), so the reported one was twice it, to four digits.
shaped-viewer-test alone on the same build passed all 503 tests, so it needs the check's other GPU binaries running beside it.
That the motion doubles rather than drifts suggests a frame counted twice, say a previous view read after a second step; this is a hypothesis.
