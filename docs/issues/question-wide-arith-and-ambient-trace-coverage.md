---
created: 2026-11-08
priority: someday
refs:
  - file: CMakePresets.json
  - file: docs/platforms.md
  - file: libs/base/typed-geometry/tests/scalar/fixed_int-test.cc
  - file: libs/base/clean-core/tests/thread/async-ambient-test.cc
  - file: libs/base/nexus/tests/test-section-test.cc
---

# Where do `SC_CHECK_WIDE_ARITH` and `SC_ASYNC_AMBIENT_TRACE` run now that no preset turns them on?

The clang debug presets no longer turn them on: they cost test time in some binaries and have not caught a real bug yet.
That was their only coverage here, so neither `dev.py check`, with or without `--all-presets`, nor CI builds them any more.

What runs only with them on:

* `TG_CHECK_WIDE_ARITH`: the `fixed_int` claim asserts, and the block of `fixed_int-test.cc` under `#if TG_CHECK_WIDE_ARITH`.
* `CC_ASYNC_AMBIENT_TRACE`: nexus's leftover-work watch, the half of `async-ambient-test.cc` that skips without it, and the trace branch of `test-section-test.cc`.

A consumer may sample them in a build dir of its own; this repository does not.
Options: a CI job that configures a debug build with both ON, on a schedule rather than every push; a preset of their own that `check --all-presets` leaves out; or nothing, accepting the gap.
