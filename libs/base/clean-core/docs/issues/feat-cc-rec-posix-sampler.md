---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/src/clean-core/record/sampling.cc
---

# A POSIX sampler

Windows suspends a thread and walks it from outside; POSIX has no equivalent.
There it wants `SIGPROF` via `timer_create`, plus a handler that walks its own stack and hands the frames and the anchor to the sampler thread.
The event layout and the splice are already shared, so only the capture half differs.
