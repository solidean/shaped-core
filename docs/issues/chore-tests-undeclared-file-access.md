---
created: 2026-11-07
priority: someday
refs:
  - file: libs/base/clean-core/src/clean-core/platform/file_access.hh
---

# Tests that list or write files past clean-core without declaring it to file-access recording

A runner using the runner report sees only what clean-core's streams open and what code declares ([external-runner](../../libs/base/nexus/docs/external-runner.md)).
These tests list directories or write files through `std::filesystem` or `std::ofstream` and declare nothing:

* shaped-graphics-language: `tests/check/checked-prelude-test.cc`, `tests/emit/spirv-emit-test.cc`, `tests/ir/sweep-ir-test.cc` (listings).
* shaped-shader-library: `tests/sgl-hit-object-spirv-test.cc` (listings), `tests/filesystem/real_filesystem-test.cc` and `tests/hot_reload-test.cc` (temp dirs).
* instruction-tracer: `tests/report/source_view-test.cc` (a temp file).

Temp files are output and matter little; a listing of a source directory is an input a cache should see.
Declaring one is a `cc::file_access::note_list` per directory listed.
