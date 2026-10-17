---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/CMakeLists.txt
  - test: shaped-graphics-test
---

# A CPU / validation backend

An always-available, in-process context — likely with no shader support at all — so the backend-agnostic tests run everywhere, not just where a real GPU backend builds.
It would make the `if(_sg_test_drivers)` guard in sg's `CMakeLists.txt` unnecessary, since a driver would always be present.
It would let large parts of the suite run on headless CI without a GPU, and double as a reference oracle to validate real backends against.
