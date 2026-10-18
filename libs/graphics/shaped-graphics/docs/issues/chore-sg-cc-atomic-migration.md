---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/.shaped-lint.yml
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/reload_generation.cc
---

# sg still names `std::atomic` / `std::memory_order` directly

About 240 occurrences, across the dx12 and vulkan backends, `raw_buffer`, `raw_texture`, `bytes_future` and `acceleration_structure` (counted 2026-10-17; the list once said 110).
clean-core has migrated to [`cc::atomic`](../../../../base/clean-core/src/clean-core/thread/atomic.hh), and `<atomic>` is no longer blessed to call into directly.
See [blessed-stdlib-headers.md](../../../../base/clean-core/docs/blessed-stdlib-headers.md).
sg's `.shaped-lint.yml` allows `<atomic>` until this lands, and `routine/reload_generation.cc` shows the target shape.

The migration is mechanical, since with threads `cc::atomic` **is** `std::atomic`.
It is load-bearing on the wasm builds without threads, where every one of those atomics keeps its interlock for a concurrency that cannot happen.
