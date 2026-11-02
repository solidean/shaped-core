---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/execute.cc
  - file: libs/base/clean-core/docs/systems/async.md
  - symbol: drive_serially
  - symbol: cc::thread_pump_all
---

# Should a parallel batch drive sweep the pump registry, as the serial one does?

`tests/execute.cc` drives a parallel batch with `cc::async_blocking_get_on`, which neither sweeps nor parks on the pumps.
`drive_serially`, in the same file, does both, so an unthreaded component a test awaits completes under `-j1` and not in parallel.
Found from the metal side, where patching the batch path took a sweep from aborting before the first test to running all 293; that case has since been settled another way.

**Sweeping there is not the obvious fix it looks like.**
[async.md](../../../clean-core/docs/systems/async.md#who-drives-a-pump) records that a second sweeping thread was tried, and raced every test that pumps its own component.
Thirteen clean-net test files pump their reactor from the test body, as do the dxc and msl shader-cache tests and sv's test environment.
They work today because nothing else sweeps during a non-main parallel phase.

The documented answer is that a test awaiting an unthreaded component runs `main_thread`, where the run loop does sweep.
What would reopen it: a component that cannot be awaited from `main_thread`, or moving the self-pumping tests to `main_thread` or `exclusive()` so the run thread may sweep everywhere.
