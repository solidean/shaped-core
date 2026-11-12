---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/raster_bound.sgl
  - file: libs/graphics/shaped-viewer/examples/openpbr-spheres.cc
---

# raster_bound shows no transmission: glass, thin-walled and scattering materials render black

`raster_bound` lights only what a surface reflects or scatters diffusely, so a fully transmissive OpenPBR surface keeps its thin Fresnel highlight and is black otherwise.
openpbr-spheres shows it on five of its sixteen rows: glass, thin walled, absorbing, dispersion and scattering, all `transmission_weight = 1`.
Fuzz draws as its dark base without the sheen, and subsurface comes out dim, since its weight replaces the diffuse lobe.

**Paused 2026-11-11 on a patent question, not on a technical one.**
The planned approach was traced refraction: the refracted ray traced through the object with the ray query, its hit lit by the secondary material and accumulated as the traced reflections are.
US 7,973,790 (Dassault Systèmes, listed by Google as active until 2027-10-20) has a broad claim 7: rasterize each polygon, and ray-trace a polygon with a reflective or refractive surface.
Read broadly it could cover traced refraction, and the traced reflections `raster_bound` already has; read narrowly, neither.
Its child US 8,013,857 expired 2025-11-14, and EA's texture-space patents (US 11,004,253, 11,620,786) do not apply to a screen-space approach.
How to read claim 7 is for counsel; the alternatives on the table were screen-space refraction over the opaque image, and refraction into the environment alone.
