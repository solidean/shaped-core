---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/test/run_tests.cc
closed: 2026-10-21
resolution: fixed
---

# A `misplaced-not` inside a `test` body was seen to pass silently

It was seen while writing the ray-tracing tests.
It does not reproduce in a plain test body: `test:` ending in `not a and b` reports it, as a function body does.
The likelier case is a test with an `error` or `warning` expectation, which takes every diagnostic inside it as its own (CHK-232).
Pin whichever it was with a corpus test.

## Resolution

It reproduces with an expectation the check never reaches: `misplaced-not` is a parse error, so the check of a test expecting, say, `type-mismatch` never runs.
The test then reported `unmet-expectation` alone, and CHK-232 swallowed the `misplaced-not` that was its reason.
A test with an unmet expectation now reports every diagnostic it took, which CHK-232 states and `expect-test.cc` pins with a check-phase warning.
The parse-phase case reaches `contain_expected` only through the driver, which `sgl test` shows; `expect-test.cc`'s harness collects no form diagnostics.
