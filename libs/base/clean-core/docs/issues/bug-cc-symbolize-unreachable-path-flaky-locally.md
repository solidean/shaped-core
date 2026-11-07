---
created: 2026-11-07
refs:
  - test: symbolize - a module on an unreachable path resolves without waiting for the network
  - file: libs/base/clean-core/tests/platform/symbolize-test.cc
---

# The unreachable-path symbolize test fails about half the time locally, at 10.3 s

What is wrong or wanted, and what is already known.

On 2026-11-07, on this Windows machine, `uv run dev.py test clean-core-test` failed this test's `seconds < 10.0` in 6 of 10 runs, every time at 10.25 to 10.49 s.
Alone, with `--repeat 3`, it takes 113 ms, so the time is spent waiting rather than working.
The test's own comment allows 10 s for queuing behind another test's symbol load on the DbgHelp lock, and a `--sample` run put 76% of its time in `SymLoadModuleExW`, under `cc::symbolizer::resolve`.

What is excluded: c2ec6db41, the crash handler's second-abort wait.
With it reverted the test failed 1 of 4 runs, and with it 5 of 6, which is no difference at these counts.

It failed in this session's ships twice today, where it had been a rare flake before.
Recent clean-core and nexus commits beside c2ec6db41 are b07eade63, 4062f5d15, d26a52b47 and 0e45e8e6b, file-access recording and the runner report's sampling, and none of them was tested against it.
Which test holds the lock, and for what, is not known.
That the 10.3 s is one constant suggests a single fixed-cost load rather than contention that varies, but that is inference.
