---
created: 2026-10-17
tags: [async]
refs:
  - file: libs/base/clean-core/src/clean-core/thread/async.hh
  - file: libs/base/clean-core/src/clean-core/thread/async_thread_pool.hh
  - symbol: cc::async_blocking_get
  - symbol: cc::try_async_blocking_get
  - symbol: cc::async_thread_pool::blocking_get
---

# `SC_NEVER_BLOCK`: a build where the blocking APIs do not exist

A whole-build switch like `SC_THREADS`, reaching the compiler as `CC_NEVER_BLOCK`: ON for every Emscripten preset, threaded ones included, and OFF elsewhere.
A native preset turns it on too, so `check` compiles the surface out on a desktop.

Under it `cc::async_blocking_get`, `try_async_blocking_get`, `async_thread_pool::blocking_get` and whatever else parks its *caller* by name are gone.
A pool worker's internal parking and plain `cc::mutex` locks stay, since neither waits for progress.

It is the deliberate exception to "no API is gated on a build switch": a documented "never call this on wasm" goes unnoticed until it deadlocks, and a missing symbol cannot.
A pthread worker may block but still should not — awaiting is strictly better — and a rule that is global is easier to hold than one that differs by thread.

The cost is a sweep: tests that block get `#if !CC_NEVER_BLOCK` or become async, and the few code paths that branch on it say so.
Meanwhile sg has no blocking spellings of its own left, and `sv::viewer`'s synchronous frame loop is the one caller of `async_blocking_get` a wasm build would reach.
