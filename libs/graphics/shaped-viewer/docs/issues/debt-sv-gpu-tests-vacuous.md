---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/pathtrace_routine.cc
  - file: libs/graphics/shaped-viewer/tests/viewer_test_env.hh
---

# Tracing tests that assert only CPU-side facts would not notice tracing nothing

**The GPU tests may still be passing vacuously.**
`pathtrace_routine`'s init used to drive its shader compiles with a throwaway single-threaded scheduler.
That could not complete a node the ambient pool already owned, so the routine ended up with no pipeline and `execute` silently no-opped.
Init is a coroutine now and awaits those compiles instead of driving them, which removes that failure mode outright.

What remains is the coverage gap it exposed: a tracing test that asserts only CPU-side facts would not notice tracing nothing at all.
Since then several tests read pixels back (`pathtraced-view-test`'s light cases, `quadric-trace-test`, `decal-trace-test`, `volumetric-furnace-test`), so the gap is narrower than it was.
The rest have not been audited.
A tracing test that means anything needs an `is_ready` assertion.
That assertion now reports the last trace rather than the routine, so it belongs AFTER the execute rather than before it.
