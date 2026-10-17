---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/fsr_backend.cc
  - symbol: sr::fsr_upscale_routine
  - symbol: sr::reconstruct_inputs
---

# FSR's reactive and transparency masks are not passed

FSR builds a default when none is given, and sr never gives one.
They matter once a tracer has transparent surfaces to report.
