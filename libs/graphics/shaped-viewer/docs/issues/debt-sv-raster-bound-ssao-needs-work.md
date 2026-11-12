---
created: 2026-11-09
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/raster_bound.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/raster_bound_routine.cc
---

# raster_bound's screen-space AO works but does not look good enough yet

`render_mode::raster_bound` has screen-space ambient occlusion since 2026-11-09, and the maintainer's verdict on it is "needs more work".

What it is: a visibility bitmask (Therrien, Levesque and Gilet 2023) over the depth prepass, at half the image's size.
Two slices of eight steps a side per pixel, accumulated over frames through the previous camera with a neighbourhood clamp, then a depth-aware 4x4 blur.
It darkens the environment's light fully and the direct light by `screen_ambient_occlusion::direct_share`.
Its parameters are `sv::render_settings::ambient_occlusion`; graphics/web-viewer's page has sliders for them.

Known to fall short:

- **Cutouts skip the depth prepass**, so the occlusion under a cutout is computed from what lies behind it.
  The modern hall's balustrade shows the steps behind it in its AO; an alpha-tested prepass for cutouts would fix it.
- **Soft and coarse**: half resolution, normals reconstructed from that depth, and a bilinear upsample with no depth weights.
- **Darkening direct light is a stand-in** for shadows, which the mode does not have yet; with shadows `direct_share` should go to 0.
- **Not checked while moving**: the accumulation's ghosting and its disocclusion handling were judged only on still frames.
- **No specular occlusion**, which the C4 plan pairs with it ([feat-sv-raster-bound-screen-space](feat-sv-raster-bound-screen-space.md)).
