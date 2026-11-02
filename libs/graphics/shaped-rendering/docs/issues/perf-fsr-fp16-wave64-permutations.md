---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/fsr_backend.cc
  - symbol: sr::fsr_upscale_routine
  - symbol: sg::feature::shader_f16
  - issue: perf-oidn-half-precision
---

# FSR runs its portable permutation only

FSR runs fp32, no forced wave64, no Lanczos table, whatever the device.
AMD tunes the 16-bit and wave64 variants for speed.
`get_device_capabilities` in the backend reports neither, so the host code never picks them.

Partly unblocked since this was written: sg now has the 16-bit gate, `sg::feature::shader_f16`, which every backend answers.
What is left is reading it here, compiling the fp16 permutation with `-enable-16bit-types`, and a wave-size answer for wave64.
The same gap the OIDN half-precision item names ([perf-oidn-half-precision](perf-oidn-half-precision.md)).
