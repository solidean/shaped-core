---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/error/result.hh
  - file: libs/base/clean-core/src/clean-core/error/optional.hh
  - file: libs/base/clean-core/src/clean-core/thread/async.hh
  - symbol: cc::result
  - symbol: cc::optional
  - symbol: cc::async
  - symbol: cc::unit
---

# Should `result<void>`, `optional<void>` and `async<void>` mean their `unit` form?

Today `void` is rejected and you spell `cc::unit` by hand: `result<void, E>` is a static_assert pointing at it, `optional<void>` has the same shape, and `async<void>` was never wired up at all.
Generic code that forms `xyz<T>` where `T` can reasonably be `void` hits this everywhere, so a per-type alias fixes one corner and leaves the rest.

The direction is to support `xyz<void>` as an affordance meaning `xyz<unit>` — same type, no conversions — decided once for `result`, `optional` and `async` together rather than type by type.

Two consequences the async side already ran into, and that the decision has to answer.
A `shared_async<void>` dependency should contribute an ordering edge and NO argument to the `make_async_*` sugar.
And it becomes indistinguishable from one deliberately carrying a `unit`.
