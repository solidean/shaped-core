---
created: 2026-10-17
refs:
  - symbol: sr::reconstruct_status::denoised
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/reconstruct.hh
---

# `reconstruct_status::denoised` is also what an upscale-only call reports

An upscale-only call that wrote its output reports `denoised` too.
A neutral name would read better once the front's other uses of "denoise" settle.
