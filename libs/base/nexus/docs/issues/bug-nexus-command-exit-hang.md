---
created: 2026-11-10
tags: [async, flaky]
refs:
  - symbol: thread_bound_scheduler::wait_for_work_or_wake
  - symbol: nx::execute_tests
  - file: libs/base/nexus/src/nexus/tests/execute.cc
  - issue: bug-cc-threaded-actor-blocking-drive-hang
---

# A command's process stayed alive after its body returned, main asleep in execute_tests, and has not come back since

A consumer's `COMMAND` printed its last line and returned 0, and the process never exited.
That happened in three runs in a row on one day, the third with almost no work in the body.
The body installed a `cc::async_thread_pool` of its own under `cc::scoped_compute_async_scheduler`, beside the pool nexus installs.
It blocked with `cc::async_blocking_get` on parallel loops: a task per worker, each taking indices from a shared counter.
Some loops ran inside workers of its pool, and their tasks blocked on further `cc::try_async_blocking_get` calls: file reads and a library's computations.

What the stacks showed, every thread attached with lldb (measured):

- main asleep in `thread_bound_scheduler::wait_for_work_or_wake` (`thread_bound_scheduler.cc:306`), called from `nx::execute_tests` (`execute.cc:2991`) under `nx::run`;
- every worker of both pools idle in `async_thread_pool::worker_main`, on its condition variable;
- the rest idle too: `cc-record`, `cc memory cap`, the crash reporter, and a library's clean-net threads, the reactor in `select` and the resolver on its inbox.

So nothing was running and nothing woke main: either the command's completion never reached main's scheduler, or its wake was lost.
The process used about one core's worth of CPU in the minutes before it was attached and almost none once it was; what used it was not captured.

**It did not come back** (measured).
The command exited with its own pool removed, and so did eight more runs after the pool was put back: short and 60 s long, through `dev.py run` and started directly.
Between the hanging runs and the clean ones only the command's data and a docs-only submodule bump changed, so the trigger is not known.

The signature is the one of [bug-cc-threaded-actor-blocking-drive-hang](../../../clean-core/docs/issues/bug-cc-threaded-actor-blocking-drive-hang.md).
There too a waiter slept in `wait_for_work_or_wake` with every worker idle, and it never came back.
Hypothesis, unconfirmed: the same lost wake, here on the main thread's scheduler that `execute_tests` drives.
It would be reached through a nested blocking drive on a pool other than the ambient one nexus installed.

**What to capture next time**, before killing the process:

- the state that issue lists: `_wake_requested`, the driver's queue, `registered_thread_pump_count()`;
- which scheduler the command's root node completed on, and whether its completion was queued to main.

lldb on Windows needs a Python 3.11 directory on `PATH` and as `PYTHONHOME` to start.
`lldb -s <file>` with `process attach --pid <pid>`, `bt all` and `process detach` gets every stack.
