---
created: 2026-11-08
priority: next
refs:
  - test: shaped-graphics-language-test
  - file: libs/graphics/shaped-graphics-language/tests/emit/spirv-emit-test.cc:278
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/lower_expr.cc:331
---

# The spirv-val fixtures test grows to 26 GB on debug-clang and fails a 23 GB allocation

"sgl spirv - every entry point of the fixtures that lowers is what spirv-val calls valid" never finishes on `debug-clang`.
Measured on Windows at `a7ba8ff45`, `uv run dev.py test shaped-graphics-language-test --preset debug-clang`, 12 jobs:

* **Default timeout (60 s):** nexus's watchdog fires after 30 s without test progress.
  The test thread is not blocked: it is in `sgl::compile_to_text` → `compile_spirv` → `lower` → `lowering::expr` (`lower_expr.cc:331`), called from `spirv-emit-test.cc:367`.
* **`--timeout 600`:** the binary fails after 181 s with peak memory 25.8 GiB.
  mimalloc cannot commit, and `cc::impl::allocation_failed` reports a single request of 23,353,884,544 bytes from inside this test.
  Every other test of the binary passes (1284 of 1285).

So it is not only slow: something in it grows without bound, and on a machine with less memory it fails sooner.
The same test was seen firing the watchdog on 2026-11-07, at an earlier commit; whether that was this growth is a hypothesis.
Not tried yet: relwithdebinfo, and which fixture and entry point is being compiled when the request is made.
