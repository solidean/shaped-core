---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-graphics-language/tests/check/autodiff-fuzz-test.cc
---

# The autodiff fuzz test failed once: 7 of 8 random programs' tests passed, seed 10386997317814370177

In one full `shaped-graphics-language-test` run on 2026-11-13, on branch jn/sgl-sm66-floor, the autodiff fuzz test failed.
That run was on the desktop, while another process held most of the machine's memory.
The test is `sgl autodiff - random programs: forward agrees with backward, and both with central differences`:

```
check failed: tested.errors == "" — "autodiff-fuzz.sgl:80:1: error: test-failed: 1 of 1 checks failed
check failed: tested.tests_passed == functions — 7 == 8 | seed: 10386997317814370177
```

at `tests/check/autodiff-fuzz-test.cc:557` and `:568`.
It failed the same way again later that day, at `autodiff-fuzz.sgl:263:1`, with seed 6851837749425501847, in a run whose other tests all passed.
Re-running the suite with `--seed 10386997317814370177` passed, so the nexus seed alone does not reproduce it.
Either the generated program depends on something else, or the failing check is at a tolerance a program sometimes misses.
Not yet known: which of the eight programs failed, and by how much forward, backward and central differences disagreed; the test does not print the program or the values.
A first step is to make the test print the program and the three derivatives of a failing check, then find a seed that reproduces it.
