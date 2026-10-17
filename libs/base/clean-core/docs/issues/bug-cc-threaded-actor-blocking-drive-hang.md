---
created: 2026-10-17
tags: [async, flaky]
refs:
  - test: "cc::threaded_actor - a post to an unthreaded actor wakes a blocking drive waiting on its reply"
  - file: libs/base/clean-core/tests/thread/thread_pump-test.cc
  - symbol: cc::threaded_actor
  - symbol: thread_bound_scheduler::wait_for_work_or_wake
---

# A blocking drive on an unthreaded actor's reply hung once, and nobody has reproduced it since

"cc::threaded_actor - a post to an unthreaded actor wakes a blocking drive waiting on its reply" (`tests/thread/thread_pump-test.cc`) timed out after 60 s in one full `dev.py check`.
The machine was starved at the time: the run took 776 s where one an hour before took 336 s.
Main was asleep in `thread_bound_scheduler::wait_for_work_or_wake`, under `async_drive_until_ready` and `async_blocking_get`; the poster thread had finished and every pool worker was idle.
It did not come back in 3000 repeats of the test or 80 of the whole binary under a concurrent full-suite load.
The hang's thread dump was overwritten before it was read in full.

**No race exists between the two threads the test has.**
The poster pushes before it notifies, the listener exists before the first sweep, and `_wake_requested` is set and read under the home's mutex and only cleared after a wait returns.

So a hang needs a third party, and three are plausible (hypotheses, none confirmed):

- A pump that blocks inside a sweep can deliver the reply through its nested drive, clear the wake, and still return false, so the outer drive parks with the reply queued.
- A hand pump holding `_is_processing` makes a sweep return false without asking for a rerun, unlike the registry's own `rerun` handshake.
- Another thread's sweep can deliver the reply onto a scheduler whose wake is then lost.

**What to capture next time**, before anything overwrites the logs: the actor's inbox size, `running` / `rerun` and `_is_processing`.
Beside them the root's state, the driver's queue, `_wake_requested`, `registered_thread_pump_count()`, and every thread's stack.
An empty inbox with the root queued on the driver is the nested sweep; the root on the pool is the third thread; a stuck `_is_processing` is the hand pump.
