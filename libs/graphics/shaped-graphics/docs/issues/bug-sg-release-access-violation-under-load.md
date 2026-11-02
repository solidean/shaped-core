---
created: 2026-10-17
tags: [flaky]
refs:
  - test: shaped-graphics-test
  - test: sg - a routine whose init fails reports failed, not pending
---

# `shaped-graphics-test` crashed once with an access violation, in the release preset under load

The sixth of twelve loaded repeats of the release suite faulted.
The binary has no symbols there, and its log was overwritten before the faulting site was known.

The crash hook named "sg - a routine whose init fails reports failed, not pending" as the only running test, and that test alone passed 400 repeats.
The faulting thread's own stack stopped at the exception dispatcher, and one pool thread was mid-work in unsymbolized frames.

Reproducing it wants a symbolized optimized build under the same load, and the log copied aside the moment a run fails.
