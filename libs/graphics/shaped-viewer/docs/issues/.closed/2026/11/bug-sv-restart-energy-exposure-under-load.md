---
created: 2026-11-02
refs:
  - test: "sv - restarting the accumulation keeps the image's mean and its automatic exposure"
  - file: libs/graphics/shaped-viewer/tests/restart-energy-test.cc
closed: 2026-11-03
resolution: fixed
---

# The restart test's exposure misses its tolerance by a little under load

What is wrong or wanted, and what is already known.

In a full `dev.py check` on the `debug-nopch-clang` preset (Windows, dx12 hardware, 2026-11-02), the worst exposure was off by 0.316 against a tolerance of 0.3, in mode 1 at frame 47.
Run alone with `--repeat 3` on the same preset it passes every time.

## Cause, measured

The failing run's mode 1 "converged" at exposure -0.07, where every passing run converges at -0.39; the 0.316 was the exposure catching up afterwards, not the restart moving it.
A trail of the exposure per frame showed why: the wavefront shades through a stand-in for up to 200 frames while its kernel compiles, and the exposure settles on that image.
The convergence wait counted those frames as drawn, so a late compile ended it a few frames after landing, with the exposure still adapting.
The wait now counts only frames that bound no stand-in.
