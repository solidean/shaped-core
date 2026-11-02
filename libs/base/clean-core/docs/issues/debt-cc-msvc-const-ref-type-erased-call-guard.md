---
created: 2026-10-17
tags: [msvc]
refs:
  - file: libs/base/clean-core/tests/function/function_ref-test.cc
  - file: libs/base/clean-core/tests/function/unique_function-test.cc
  - file: docs/bugs-external/msvc-const-ref-through-type-erased-call/readme.md
---

# Drop the MSVC 19.51 value-check guards once the compiler fix ships

MSVC `cl` >= 19.51 at `/O2` miscompiles returning a reference to an odr-used const local through a type-erased call, so the reference comes back dangling.
[docs/bugs-external/msvc-const-ref-through-type-erased-call](../../../../../docs/bugs-external/msvc-const-ref-through-type-erased-call/readme.md) is the reproduction and the matrix.

`function_ref-test.cc` and `unique_function-test.cc` each quarantine one runtime value-check behind `_MSC_VER >= 1951`; the type assertion beside it still runs everywhere.
Filed upstream: <https://developercommunity.visualstudio.com/t/MSVC-O2-miscompiles-returned-const-loca/11114080>

When a fixed toolset ships, bound the guard's version range to the broken ones, or drop it once the fixed toolset is the floor.
