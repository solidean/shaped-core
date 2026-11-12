---
created: 2026-11-09
tags: [routine]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/render_routine.cc
---

# A phase abandoned by a reload can still mark the next run failed

`fail_init()` sets the routine's `_failed` directly, with no tie to the run that called it.
A reload drops the `init` it cancels, but that coroutine keeps running until its next `with_cmd` notices the cancellation.
If it calls `fail_init()` after the new generation's run started, and that run then succeeds, the routine publishes ready with `_failed` still set, and reads as failed until the next reload.

Found by reading in review on 2026-11-09; not reproduced.
The fix is to record a failure on the run (`impl::routine_run`) and copy it into `_failed` only when that run is collected.
