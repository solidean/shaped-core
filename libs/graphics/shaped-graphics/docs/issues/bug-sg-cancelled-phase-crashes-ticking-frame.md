---
created: 2026-11-09
tags: [routine, async]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/routine_registry.cc
  - test: "sg - a routine cancelled at its own generation is not restarted"
---

# A routine phase that ends cancelled crashes the process when the tick runs in a test's own frame

Seen 2026-11-09 on `sg dx12 hardware backend`, relwithdebinfo.
A routine whose `init` awaits `cc::make_async_from_error<cc::unit>(cc::async_error::make_cancelled())` was ticked by `ctx.routines.tick(...)` called straight from an ASYNC_INVOCABLE_TEST body.
The process stopped on a `__debugbreak` with no assert message, its stack inside the test's coroutine frame.
The same ticks run inside a node of their own (`cc::make_async_scheduled`), as `idle_completion` runs them, pass; a phase ending in an ordinary error passes either way.

Not yet known: what breaks.
The suspicion is that a phase started from the caller's frame inherits its async context, so the phase's cancellation reaches the caller's frame; unverified.
`sv::viewer::begin_frame` ticks the routines from the caller's frame, as graphics/web-viewer's coroutine loop does, so the crash is not confined to tests.
The test above ticks inside its own node as a workaround; ticking from its body reproduces this.
