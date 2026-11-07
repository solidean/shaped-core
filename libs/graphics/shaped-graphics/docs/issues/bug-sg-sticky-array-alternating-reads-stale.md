---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/tests/barrier/sticky-array-test.cc
  - test: "sg - groups alternating A, B, A at one array name re-apply where the other moved a layout"
---

# A sticky-array test read 1.8e-43 where each thread wrote 0.25 or 0.75, once in about 75 iterations

What is wrong or wanted, and what is already known.

On 2026-11-07 `--repeat 20` failed once on iteration 5 on the Intel Arc B580, on the `vulkan` context, at 3b9189646, which is before native `print`.
Every thread's value read 1.8e-43, which is the bits `0x80`, where the shader writes 0.25 or 0.75.
All of `got_a` and `got_b` read that, so this is a whole read gone wrong rather than one group's layout.

The test passed in every other iteration of that comparison, roughly 150 on both sides, and it was the only check failure among them.
It was a clean test failure with exit code 1, with no device loss logged, so it does not look like [bug-sg-vulkan-device-lost-under-repeat](bug-sg-vulkan-device-lost-under-repeat.md).
Why it reads that value is unknown, and `--seed` was not retried.
The failed run's logs are in the build's `crashes/shaped-graphics-test-20261107-061430-failed`, which a clean build removes.
