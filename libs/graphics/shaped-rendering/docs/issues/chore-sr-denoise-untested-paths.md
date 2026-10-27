---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/tests/denoise-test.cc
  - symbol: sr::reconstruct_history::_prepare
  - symbol: sr::atrous_denoise_routine::options_for
  - symbol: sr::svgf_denoise_routine::options_for
---

# Two denoise tests worth having and not written yet

- **A method switch on one history** — à-trous then SVGF on the same `sr::reconstruct_history`.
  It is the one branch of `reconstruct_history::_prepare` nothing covers.
- **`options_for` on both members** beyond the defaults.
  It maps `quality` and `sharpness` onto pass counts and sigmas, and is what any settings UI drives.
  `sr - options_for at default settings equals the member's own defaults` pins only the default settings.
