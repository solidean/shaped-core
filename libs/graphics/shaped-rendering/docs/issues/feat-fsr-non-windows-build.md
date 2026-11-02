---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/fsr_backend.cc
  - file: extern/fidelityfx/CMakeLists.txt
  - symbol: sr::fsr_upscale_routine
---

# FSR builds on Windows only

`SR_HAS_FSR` is Windows-only today.
AMD's host code calls MSVC's secure C runtime (`wcscpy_s`, `getenv_s`, `strncpy_s`) and uses `__declspec` unconditionally.
A force-included compat header would widen it, and nothing here has built it against another C runtime yet.
