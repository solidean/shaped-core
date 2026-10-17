---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/light.hh
---

# An emissive mesh lights nothing but the camera

`emission_luminance` is authored and shaded, but next-event estimation samples the analytic lights alone.
So emissive geometry is direct-visibility only and contributes no indirect light.
What it needs is light sampling over emissive triangles: an emitter list built per trace with its own area pdf, balanced against the BSDF sampler the way a rect light already is.
