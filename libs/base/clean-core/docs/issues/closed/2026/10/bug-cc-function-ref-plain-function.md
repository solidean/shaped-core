---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/function/function_ref.hh
  - symbol: cc::function_ref
  - file: libs/data/babel-serializer/tests/trace/chrome_trace-test.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-18T05:58Z
closed: 2026-10-18
resolution: fixed
---

# `cc::function_ref` does not accept a plain function portably

It stores its target as `void*` and casts it back to `Fn*`, and a function pointer to `void*` is only conditionally supported.
clang-cl accepts `cc::function_ref<void()>(my_function)`; clang on Linux, macOS, iOS and wasm rejects it, so the mistake builds on Windows and breaks CI elsewhere.

The fix is a separate function-pointer member (or a union with the object pointer) chosen at construction, plus a test that passes a function.

Until then a caller wraps it: `libs/data/babel-serializer/tests/trace/chrome_trace-test.cc` passes `[] { busy_round(); }` for exactly this reason.
