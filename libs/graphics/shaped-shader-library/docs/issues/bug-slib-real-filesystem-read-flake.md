---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-shader-library/tests/filesystem/real_filesystem-test.cc
  - test: slib - real_filesystem reads files under its root
---

# real_filesystem's read test fails now and then under a full run

Seen once on 2026-11-09 in a full `dev.py ship` run on Windows: `fs.read_text("a.hlsl").value()` found no value, an empty optional.
Run alone 30 times with `--repeat`, it passed every time; the failure ran beside other slib tests.

The test writes three files into a fresh directory under the OS temp directory and reads them straight back.
Not yet known why a read right after the write can miss.
A guess, not measured: a scanner holding a new file open, which a read with no retry sees as a missing file.
