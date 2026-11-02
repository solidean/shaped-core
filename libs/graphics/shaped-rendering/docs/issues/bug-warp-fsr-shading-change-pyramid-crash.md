---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/fsr_upscale_routine.cc
  - file: libs/graphics/shaped-rendering/shaders/fsr3/ffx_fsr3upscaler_shading_change_pyramid.h
  - file: libs/graphics/shaped-rendering/shaders/fsr3/shading_change_pyramid.hlsl
  - symbol: sr::fsr_upscale_routine
  - test: sr - fsr is refused on a software adapter
  - issue: debt-fsr-microsoft-vendor-warp-check
---

# WARP crashes executing FSR 3.1's shading-change pyramid pass

**WARP crashes executing FSR 3.1's shading-change pyramid pass**, inside `d3d10warp`'s own worker thread; the hardware runs it correctly.
`fsr_upscale_routine` refuses a software adapter until this is understood, so the FSR image tests skip there.
It is a question for sg's CI legs first, since the dx12 "hardware" leg is a WARP-backed adapter on those runners ([debt-fsr-microsoft-vendor-warp-check](debt-fsr-microsoft-vendor-warp-check.md)).

What bisecting it established, each by running it:
- Skipping that one pass, and only that one, avoids the crash; an empty entry point avoids it too.
- Its stores into FSR's downsample image are what trigger it: with them removed the pass runs.
- It is not an out-of-bounds write: a bounds check on the store does not help.
- Nor is it the SPD wave path (`FFX_SPD_NO_WAVE_OPERATIONS` does not help) or the image's format (`rg32_float` does not help).
- Nor the view clamp below 64 pixels: a 128-pixel input crashes the same.
- Declaring the mip views `float4` rather than `float2` avoided it only while other parts of the pass were stubbed out.

So the fault is in WARP compiling this particular shader.
The next step is a reduced shader outside FSR that still crashes, to report or to route around.
