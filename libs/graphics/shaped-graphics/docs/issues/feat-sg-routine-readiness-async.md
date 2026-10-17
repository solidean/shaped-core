---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/render_routine.hh
  - file: libs/graphics/shaped-viewer/tests/viewer_test_env.hh
  - file: libs/graphics/shaped-rendering/tests/raster-box-filter-mipmap-test.cc
---

# Readiness as an async, so the GPU tests stop working around it

The suites are async: the entry drivers await `nx::async_invoke_tests_in_sequence`, and the tests await `ctx.idle_completion()`.
Two workarounds remain, both waiting on a readiness signal rather than on the migration.

- A tick drives only routines already registered, so a test prewarms each variant by name before asserting on its first frame.
  A routine's readiness as an async would let it await instead.
- sv's `frames_until_executed` and the furnace loop re-record frames until a path-traced state object lands.
  The same signal for that would turn both into one await.

What still blocks otherwise — manual window loops, fuzz steps, a few synchronous compile helpers — is allowed by file in each library's `.shaped-lint.yml`.
The tests that prewarm or re-record carry an `issue:` marker naming this.
