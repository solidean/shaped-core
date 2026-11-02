---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/render_routine.hh
  - file: libs/graphics/shaped-graphics/docs/render-routines.md
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/impl/device_lifecycle.cc
  - symbol: sg::render_routine
---

# Render routines want a shared/exclusive lock, not a mutex

The model to reach: a routine's init phases exclude every `execute`, while `execute` calls that only *read* run in parallel with each other.
A read-only routine like `sr::blit_routine` — one that can be acquired without exclusivity — has no reason to serialize against another thread's `execute`.

Both halves are approximated today, because clean-core has no shared/exclusive mutex:

- `try_acquire()` takes **no** lock where it wants a shared one.
- `try_acquire_exclusive()` serializes `execute` calls that would be free to overlap.

**The clean-core extension it needs is a `cc::shared_mutex<T>` next to `cc::mutex<T>`** — `lock_shared(f)` / `lock_shared_scoped()` alongside `lock(f)` / `lock_scoped()`.
Then `try_acquire()` holds a shared guard for the caller's read and `try_acquire_exclusive()` keeps the exclusive one.
`device_lifecycle.cc` includes `<shared_mutex>` directly for the same missing type, allowed by name in sg's `.shaped-lint.yml`, and moves over with it.

The phases are a separate question: they are coroutines and can hold no lock at all across a suspend.
What excludes them from an `execute` today is that they run only inside a tick, which is a frame-boundary call.
See [render-routines.md](../render-routines.md#threading).
