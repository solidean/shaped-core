---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/fsr_upscale_routine.cc
  - symbol: sr::fsr_upscale_routine
  - symbol: sg::adapter_info::is_software
  - issue: bug-warp-fsr-shading-change-pyramid-crash
---

# FSR refuses every Microsoft-vendor adapter, not only software ones

`fsr_upscale_routine` refuses a software adapter, because of [bug-warp-fsr-shading-change-pyramid-crash](bug-warp-fsr-shading-change-pyramid-crash.md).
It also refuses any Microsoft-vendor adapter (vendor id `0x1414`).
A GPU-less CI runner exposes one that DXGI does not flag as software, and it renders through WARP all the same.
So the dx12 "hardware" leg of sg's CI is that adapter on those runners.

The vendor check is temporary: sg's `adapter_info::is_software` reporting such an adapter is what replaces it.
That half is sg's work; once it lands, `renders_through_warp` reduces to `is_software`.
