---
created: 2026-11-09
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/raster_bound_routine.cc
  - file: examples/graphics/web-viewer/web_viewer.cc
---

# raster_bound lights nothing from an emissive instance, which is how glTF scenes carry their lights

The tracer lights a scene from its emissive instances (docs/lights.md#emissive-instances); `render_mode::raster_bound` shades only with the scene's lights and its background.
A glTF scene converted from a path tracer carries its light as emissive meshes and nothing else.
The Research Scenes' modern hall is the case at hand: 13 emitters, 0 lights, so the bound mode lights the room evenly from the background and it reads gray.

The web-viewer works around it by turning each emissive mesh into a rect light of the same plane, area, radiance and color, once at load.
That is marked `TEMPORARY(feat-sv-raster-bound-emissive-instances)`; it belongs in sv, where the raster layer could derive the same lights from the instances it already resolves.
A disc becomes a square of its area, and a curved emitter has no good rect at all, which the workaround does not try to handle.

## Log

2026-11-12: one bounce of it works.
A `raster_bound` layer now gets each emissive instance's light record, its estimated radiance without the area CDF a tracer samples by, and the traced indirect brings back the emission of an emitter its ray meets.
The Research Scenes' kitchen, White Room, Modern Hall and staircase went from near black to lit.
What stays: a surface that sees no emitter gets nothing, since the hit a ray meets is lit by the analytic lights alone, so the Cornell box's short box faces away dark.
A second bounce, from what last frame left at the hit where it is on screen, is the next step.
