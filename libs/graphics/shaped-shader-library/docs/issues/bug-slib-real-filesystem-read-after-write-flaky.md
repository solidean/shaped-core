---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-shader-library/tests/filesystem/real_filesystem-test.cc
  - test: "slib - real_filesystem revision moves when a file's content changes"
---

# A read right after a rewrite came back empty, once, under a full gate

Seen 2026-11-12, locally on Windows, in `dev.py ship`'s full test run, with the GPU suites running beside it.
`fs.read_text("a.hlsl")` returned no value right after `dir.write` rewrote the file, so `.value()` asserted at the test's line 111.
The rerun passed, and the test passed alone.

Hypothesis, unverified: a transient sharing violation, another process such as an indexer or a scanner holding the freshly written file.
`real_filesystem::read_text` reports why it failed nowhere the test sees, so the next sighting cannot tell that from anything else.
First step: have the test print the read's error where there is one, then retry a sharing violation once in `read_text`, if that is what it is.
