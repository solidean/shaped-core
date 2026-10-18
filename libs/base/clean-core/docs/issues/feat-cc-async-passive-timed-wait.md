---
created: 2026-10-17
tags: [async]
refs:
  - file: libs/base/clean-core/src/clean-core/thread/async.hh
  - symbol: cc::try_async_blocking_get_for
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/impl/sgl_emit_cache.cc
---

# A passive timed wait on a `cc::async` node

Waiting until a node is ready or a deadline passes, while running nothing else on the calling thread, has no spelling.
`cc::try_async_blocking_get_for` is the timed wait there is, and it drives: it steps the ambient scheduler and sweeps the pump registry until the node is ready.
A caller that may hold a lock, or a reference other work could invalidate, must not run that work, so it polls `is_ready()` by hand instead.

slib's `passive_get` (`impl/sgl_emit_cache.cc`) is the first, waiting on the blob cache's actor thread with a yield-then-sleep loop marked as this issue's stand-in.

The shape wanted parks on the node's completion, woken by it the way a parked driver is, rather than polling, and takes its bound in seconds.
It can only ever see work another thread finishes, so without threads it has nothing to wait for; the SGL cache switches its persistent tier off there.
