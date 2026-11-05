---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - test: sv::hybrid_routine - a glass pane thinner than a millimeter shows the sky through it
---

# Intel's dx12 driver stalls creating a raster pipeline of sv's guarded shaders

Under `report` shaped-viewer-test timed out at 170 s on 2026-11-05, awaiting `sv::hybrid_routine - a glass pane thinner than a millimeter shows the sky through it`.
The stuck thread was inside igd12um64xe2.dll under CreateGraphicsPipelineState (dx12_raster_pipeline.cc:203), on an Intel Arc B580 whose driver also logs hundreds of timeouts a day since 2026-10-19.
The guards add a branch per division and index to sv's large shaders; whether the driver's compiler is slow on that or truly hangs is not known.
Unguarded, the same binary passes in 54 s.
Worth a retry on another driver or vendor before reading more into it.
