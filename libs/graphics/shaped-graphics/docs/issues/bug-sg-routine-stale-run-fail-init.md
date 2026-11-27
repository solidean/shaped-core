---
created: 2026-11-09
tags: [routine]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/render_routine.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-27T14:41Z
---

# A phase abandoned by a reload can still mark the next run failed

`fail_init()` sets the routine's `_failed` directly, with no tie to the run that called it.
A reload drops the `init` it cancels, but that coroutine keeps running until its next `with_cmd` notices the cancellation.
If it calls `fail_init()` after the new generation's run started, and that run then succeeds, the routine publishes ready with `_failed` still set, and reads as failed until the next reload.

Found by reading in review on 2026-11-09; not reproduced.
The fix is to record a failure on the run (`impl::routine_run`) and copy it into `_failed` only when that run is collected.

## Resolution

`fail_init` takes the phase's scope and sets a flag on that run, which `advance_init` takes in when it collects the run.
A run a reload replaced is never collected, so its failure reaches nothing that is still current.
`sg - an init a reload replaced fails only itself, not the run that replaced it` fails the run-one scope after run two published; the old flag read failed there.

The window is narrower than the issue said: a phase a reload drops while it is suspended is cancelled at that await, and never resumes.
Only a phase running between two suspensions when the reload lands could fail late, which the test models by keeping the first run's scope.
