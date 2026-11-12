---
created: 2026-10-17
tags: [rec]
---

# A Linux and Android sampler

Windows suspends a thread and walks it from outside, and macOS does the same through Mach (`thread_suspend`, `thread_get_state`).
Linux and Android have no equivalent, so `cc::rec::sampling_available()` is false there.
There it wants `SIGPROF` via `timer_create`, plus a handler that walks its own stack and hands the frames and the anchor to the sampler thread.
The event layout and the splice are already shared, so only the capture half differs.
`tests/record/record-sampling-test.cc` covers the shared half and runs wherever sampling is available.
