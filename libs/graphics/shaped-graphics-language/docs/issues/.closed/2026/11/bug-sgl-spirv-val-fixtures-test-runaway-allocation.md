---
created: 2026-11-08
priority: next
refs:
  - test: shaped-graphics-language-test
  - file: libs/graphics/shaped-graphics-language/tests/emit/spirv-emit-test.cc:278
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/lower_expr.cc:331
closed: 2026-11-26
resolution: obsolete
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

## Log

**2026-11-25: it does not reproduce on Linux at `26216f392`.**
Run under an 8 GB cgroup cap with no swap, on a Steam Deck (16 GB):

* `debug-linux-clang`: passes in 19.1 s, 4794 checks, peak 339 MiB.
* `relwithdebinfo-linux-clang`: passes in 1.4 s, peak 295 MiB.

Whether it was fixed in the 949 commits since `a7ba8ff45`, or happens on Windows alone, is open.
None of those commits names this growth.
Next is a run of `debug-clang` on Windows at HEAD; if it still grows there, the request's stack names the fixture and the entry point.

**2026-11-26: it does not reproduce on Windows either, at `3dd4c682d`.**
`uv run dev.py test "every entry point of the fixtures that lowers is what spirv-val calls valid" --preset debug-clang --timeout 600`:
it passes in 15.5 s, 4794 checks, peak 367 MiB.
So it was fixed somewhere in the commits since `a7ba8ff45`, by a change that named no growth; closed as obsolete.
