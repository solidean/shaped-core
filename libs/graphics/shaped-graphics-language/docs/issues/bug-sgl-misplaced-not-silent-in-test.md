---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/test/run_tests.cc
---

# A `misplaced-not` inside a `test` body was seen to pass silently

It was seen while writing the ray-tracing tests.
It does not reproduce in a plain test body: `test:` ending in `not a and b` reports it, as a function body does.
The likelier case is a test with an `error` or `warning` expectation, which takes every diagnostic inside it as its own (CHK-232).
Pin whichever it was with a corpus test.
