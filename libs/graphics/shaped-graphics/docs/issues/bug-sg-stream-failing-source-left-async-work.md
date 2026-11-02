---
created: 2026-11-02
refs:
  - test: sg stream - a failing source settles the transfer on its error channel
  - file: libs/graphics/shaped-graphics/tests/transfer/stream-test.cc
---

# A stream test of a failing source sometimes ends with its transfer's async work still running

What is wrong or wanted, and what is already known.

On the vulkan backend in a full `dev.py test` of shaped-graphics-test (Windows, 2026-11-02), nexus reported that the test "left async work running — 1 async item(s) still carry its context".
Run alone with `--repeat 30` it passes every time, so it depends on load; the test likely ends before an error-channel continuation of the transfer has run.
