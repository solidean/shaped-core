---
created: 2026-11-24
priority: normal
tags: [sg, metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_context.cc
  - symbol: sg::feature::norm16_filtering
---

# Metal grants `norm16_filtering` on every GPU, unverified

sg's metal backend answers `sg::feature::norm16_filtering` true on every device: a sampled texture of `r16_unorm`, `r16_snorm`, `rg16_unorm`, `rg16_snorm`, `rgba16_unorm` or `rgba16_snorm` may be filtered.
That rests on Apple's Metal feature-set tables, read from memory, which give the 16-bit normalized formats the filter capability on the GPUs the backend's Metal 4 floor admits.
Nobody has checked it, and Metal has no run-time query of a format's capabilities, so a wrong answer shows only as a filtered texture that comes out wrong, or a validation error.

## Wanted

- Checked on a Mac, against the current Metal feature-set tables and on a device: which Apple GPU families filter each of the six, the `snorm` ones included.
- If any family the backend admits does not filter one of them, `norm16_filtering` answers per family there, and the comment beside the answer says which.
- A device test that samples each of the six through a linear sampler on metal and checks the filtered value, beside the dx12 and vulkan ones.
