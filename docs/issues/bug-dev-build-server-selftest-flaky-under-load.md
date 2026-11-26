---
created: 2026-11-26
tags: [dev]
refs:
  - file: tools/dev/build-server-self-test.py
---

# build-server-self-test fails under the gate's parallel load

Seen once in `check` (from a consuming project's `ship`), on Windows, while the dev-selftest gates ran about 17 self-tests side by side and other builds held most of the machine's memory.
It passed 42 of 42 alone, and in four concurrent runs alone.
Six cases failed together: after the first case whose build fails on purpose, every later fake build was reported `not started: all failed 10 s before`.
So the fail-fast state of one case reached the cases after it.
`test_background_test_runs_are_asked_for_only_when_the_project_says_so` then failed with an `IndexError` on `f.calls()[0]`, since nothing was called.
The likely seam is the process-wide fail-fast state not being reset between cases when a case outlives its grace under load.
