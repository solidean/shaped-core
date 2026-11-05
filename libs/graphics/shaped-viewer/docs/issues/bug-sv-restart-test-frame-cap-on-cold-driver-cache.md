---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-viewer/tests/restart-energy-test.cc:147
  - test: "sv - restarting the accumulation keeps the image's mean and its automatic exposure, in the megakernel"
---

# The megakernel restart test runs out of frames while a cold driver cache compiles its stand-ins

On 2026-11-05 a change to the text of every sv shader that compares floats (EMIT-186) made the next run compile each pipeline cold in Intel's dx12 driver.
The restart test then reached `++frames < max_frames` at 1024 frames while its stand-ins were still compiling, failed, and left async work running; shaped-viewer-test took 212 s instead of about 50.
Run again with the driver's cache warm, the same binary passed in 64 s.
The test waits on a frame count where it means to wait on its pipelines being ready, so a slow cold compile reads as a failure.
