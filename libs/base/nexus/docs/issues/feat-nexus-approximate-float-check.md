---
created: 2026-11-22
refs:
  - file: libs/base/nexus/src/nexus/tests/check.hh
---

# nexus has no approximate comparison for floats

A test of float results writes its own comparison against a tolerance today, and every one picks its tolerance differently.
The first project started on shaped-core asked for one in its first week.

Wanted: a check that two values agree within a tolerance, relative or absolute, which dumps both values and the gap when it fails.
Open: its spelling (a matcher inside `CHECK`, or a function returning a dumpable result), and whether ULPs are a third way to state the tolerance.
