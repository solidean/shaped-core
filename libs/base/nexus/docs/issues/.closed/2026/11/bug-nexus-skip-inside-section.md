---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/check.hh
  - symbol: SKIP
closed: 2026-11-03
resolution: fixed
---

# `SKIP` does not interact with `SECTION` properly

The macro's own comment says so, without saying how.
Not reproduced yet: the first step is a test that skips inside a section and pins what the run should report.

Fixed 2026-11-03, measured first: a SKIP in a section ended the whole test, so the sections after it never ran and the test read as skipped.
Now the replay runs one more pass after such a skip, which finds the sections after it, and a test is reported skipped only when nothing in it ran but skips.
test-section-test.cc pins both.
