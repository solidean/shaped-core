---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/check.hh
---

# `CHECK(a == b && c)` has no guardrail

The expression capture decomposes the comparisons, and has nothing for `&&` and `||`.
Catch2 rejects a chained logical expression at compile time, since it cannot decompose it; nexus wants the same refusal.
What such an expression does today is not pinned by a test.
