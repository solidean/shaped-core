---
created: 2026-11-02
refs:
  - test: "sv - restarting the accumulation keeps the image's mean and its automatic exposure"
  - file: libs/graphics/shaped-viewer/tests/restart-energy-test.cc
---

# The restart test's exposure misses its tolerance by a little under load

What is wrong or wanted, and what is already known.

In a full `dev.py check` on the `debug-nopch-clang` preset (Windows, dx12 hardware, 2026-11-02), the worst exposure was off by 0.316 against a tolerance of 0.3, in mode 1 at frame 47.
Run alone with `--repeat 3` on the same preset it passes every time, so the tolerance sits close to what a loaded machine's frame timing gives.
