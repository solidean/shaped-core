---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/bytes_future.hh
  - symbol: sg::bytes_future
  - symbol: sg::gpu_timestamp
---

# Directly awaitable futures, if `co_await future.data()` ever reads as noise

`bytes_future`, `data_future<T>` and `gpu_timestamp` hand out their result through `bytes()`, `data()` and `ticks()`, each a `cc::shared_async` built from `completion()`.
An `operator co_await` on the future types would let a caller write `co_await future` instead.
It was left out so sg's value types carry no coroutine machinery, and it is additive whenever it is wanted.
