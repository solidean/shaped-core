---
created: 2026-11-11
refs:
  - file: libs/base/clean-core/src/clean-core/record/sampling.cc
closed: 2026-11-11
resolution: fixed
---

# record.thread_run on macOS: a thread's running time per sampler tick, from thread_info

The sampler records `record.thread_run` on Windows only: per tick and thread, how much of the stretch since its last tick the thread ran.
On Windows that is QueryThreadCycleTime against the timestamp counter, read before the suspend, with `in_scope` read during it.
Wanted the same on macOS: `thread_info(port, THREAD_BASIC_INFO)` gives `user_time` and `system_time`, in microseconds, for the Mach port the sampler already holds.
`interval` and `ran` must then share a unit there too, nanoseconds or microseconds, which the field docs in sampling.hh would say per platform.
Not done because it cannot be tested from the Windows machine it was written on (2026-11-11).
Linux has no sampler yet, so it waits for that.

## Resolved

The sampler reads `thread_info(port, THREAD_EXTENDED_INFO)` on macOS: user and system time in nanoseconds, converted to the counter's ticks at `rec::cycles_per_second()`.
So `ran` and `interval` keep one unit on both platforms, which babel::chrome_trace needs, since it subtracts `interval` from the event's cycle stamp.
The extended info costs the same as THREAD_BASIC_INFO, about 340 ns a call on an M4, at a thousand times its resolution.
The Apple walk never filled `in_scope` until now; it reads the thread's open scope as the Windows walk does.
The thread_run test now runs on macOS too, and passed six runs out of six on an M4.
