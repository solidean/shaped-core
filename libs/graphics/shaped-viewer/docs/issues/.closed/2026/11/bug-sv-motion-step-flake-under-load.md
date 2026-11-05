---
created: 2026-11-05
refs:
  - test: "sv - a view stepping sideways in front of a wall reports the step as every pixel's motion"
closed: 2026-11-05
resolution: fixed
---

# sv's sideways-step motion test reports twice the expected motion once under parallel GPU load on an M4

In `dev.py ship`'s check on 2026-11-05, on macos-arm-llvm-relwithdebinfo, the metal megakernel run failed with `worst < 0.02` at 0.2217.
The expected motion was 0.1109, and the centre pixel moved 0.3326, three steps to four digits; 0.2217 is the error.
shaped-viewer-test alone on the same build passed all 503 tests, so it needs the check's other GPU binaries running beside it.
A hypothesis: the megakernel path records `last_camera` only after a trace that did not decline, so two declined frames leave the next traced one measuring from three steps back.
That is right for a temporal member, whose history was rendered there, and the test cannot tell, since a layer's decline does not reach `viewer_renderer::execute`'s outcome.
It did not reproduce in seven rounds of the GPU binaries run together on 2026-11-05.

**The test read guides from frames that declined.**
Instrumented, a warm megakernel still declines frames while a compile is pending, and on those the guide slot keeps what it held.
The first traced frame after declines measures from the last traced camera, which is right for a temporal member: two declines make three steps.
The test counted the stale guides of the declined frames after it as settled, so its last guide was that three-step one.
It now counts a guide only from a frame that traced after a frame that traced, told apart by the slot's `last_camera`, which a layer updates only when it traces.
