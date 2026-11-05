---
created: 2026-11-04
refs:
  - test: "async home - a push to a home always wakes its owner parked in a pool, however it races the park"
---

# The async-home push/park race test sometimes hangs the whole clean-core-test run

Seen 2026-11-04 on relwithdebinfo (Windows, clang): one of three full clean-core-test runs hung until dev.py killed it at 60 s.
The running test was this one, and its race_one_round thread sat in participate_until_ready (async-home-test.cc:584) on a condition variable.
So a wake was likely lost between the push and the park.
The hang report and minidump were kept under build/x64-windows-clang-ninja-relwithdebinfo/crashes/clean-core-test-20261104-135522-60752-2.
The test and the pool's broadcast slot both date from 2026-10-29.
