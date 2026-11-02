---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/check.hh
  - symbol: SKIP
---

# `SKIP` does not interact with `SECTION` properly

The macro's own comment says so, without saying how.
Not reproduced yet: the first step is a test that skips inside a section and pins what the run should report.
