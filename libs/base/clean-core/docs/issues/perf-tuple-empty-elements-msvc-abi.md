---
created: 2026-11-06
priority: someday
refs:
  - file: libs/base/clean-core/src/clean-core/container/tuple.hh
  - symbol: cc::impl::tuple_leaf
---

# cc::tuple: an empty element still takes space under the MSVC ABI

Each element is a tuple_leaf base holding it as a CC_NO_UNIQUE_ADDRESS member.
Under the Itanium ABI a leaf around an empty type is itself empty, so it costs nothing.
Under the MSVC ABI it does not: a leaf around an empty type is not an empty base there, and `__declspec(empty_bases)` does not help either, measured with clang-cl 22.
`sizeof(cc::tuple<double, empty>)` is 16 under clang-cl and cl.exe 14.44; the same leaves give 8 under the Itanium ABI, measured with clang 22 on a standalone copy of them.
The two Windows compilers also disagree with each other: `sizeof(cc::tuple<empty, int>)` is 8 under clang-cl and 4 under cl.exe.

A likely fix is the classic one: a leaf around an empty, non-final T derives from T instead of holding it, and the storage carries `__declspec(empty_bases)` under `_MSC_VER`.
That changes how get<I> reaches the element and what names leak into the leaf, so it needs its own design pass and a layout test per compiler.
