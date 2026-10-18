---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/execute.cc
  - symbol: drive_serially
  - symbol: cc::thread_pump_all
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-18T05:59Z
---

# The parallel batch drive never sweeps the pump registry, so an unthreaded graph cannot complete on it

`tests/execute.cc` drives a batch with `cc::async_blocking_get_on`, which neither sweeps nor parks.
`drive_serially`, in the same file, does both — and its comment states exactly why a drive that skips them cannot complete a graph waiting on a semantic thread that has no thread of its own.
So the two paths disagree, and the parallel one is wrong.

Any unthreaded component driven by a registered pump hits this: clean-net's reactor, and sg's own `pump_completion_signals`.
Found from the metal side, where patching the batch path took a sweep from aborting before the first test to running all 293.
That case has since been settled another way, but the nexus bug is unchanged.

Giving the batch path `drive_serially`'s loop is the fix.
