---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-viewer/examples/lights.cc
  - file: libs/graphics/shaped-viewer/examples/light-stage.cc
---

# raster_bound overexposes the lights examples' daylight panel and light-stage's wall

Captured natively with `SV_AUTOMATIC_MODE=raster_bound` on 2026-11-11, and shown the same on the web pages:

- lights: the daylight panel is blown out to white, and the sun and spot panels' spheres carry blotchy bright speckles.
- light-stage: the wall behind the props is overexposed, and its light comes out in large blotches.

The traced modes draw both examples as intended, so it is `raster_bound`'s lighting of those lights, or its exposure, rather than the scenes.

research-studio showed it too, in its daylight rooms (breakfast room, classroom, house).

## Cause

`raster_bound_shadow_maps.light_geometry` weighed a distant disc, a sun, by its cosine alone.
A disc's emission is radiance, which `raster.direct_light` multiplies by the cone it subtends, so the traced indirect lit every sunlit bounce thousands of times too bright.
It now weighs the disc by that cone too.

A second, smaller cause sat in the automatic exposure: the black frames while the shaders compile primed it ten stops bright, and it took many frames to adapt back.
`meter_auto_exposure` now skips a reading at the meter's floor, `sr::exposure_meter_routine::darkest`.

Fixed in the working tree; captured natively on 2026-11-11, lights, light-stage and the three rooms all metered as the hybrid meters them.
