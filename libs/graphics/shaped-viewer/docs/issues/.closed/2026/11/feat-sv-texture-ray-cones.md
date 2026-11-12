---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
closed: 2026-11-11
resolution: fixed
---

# Every texture is sampled at mip 0

A footprint along the ray (ray cones) is what picks the level.
On Intel's Sponza (the `sponza` example) it is what the megakernel's frame is left paying for: about 470 ms at 1920x1080 on an RTX 5070 Laptop GPU, against the wavefront's 150 ms.
BC7 compression already took the scene from 7.7 GB, over the device's budget, to 3.1 GB; the wavefront's frame went from over 800 ms to 150 ms with it.

## Resolution

A hit samples a texture at its ray cone's level (Akenine-Moller et al. 2019), in the megakernel's closest hits and the wavefront's shade kernels alike.
- **The cone:** a pixel's angle wide from the camera (`frame_constants::pixel_spread`), carried by the payload and the wavefront's path state.
  It widens by its spread times each segment's length.
  Its spread widens by the lobe a bounce leaves along: 2 alpha for a specular lobe, a radian for a diffuse one or a medium's scatter.
- **The level:** `shading_context::lod` holds the cone's width over its cosine against the triangle's world area.
  The generated load adds half the log2 of the texels the triangle's uvs span.
- **Kept at level 0:** emission and opacity, which next-event estimation and the any-hit read with no cone; a light seen blurred by a bounce would weigh against one that is not.
- **Left out:** a surface's curvature, so a curved mirror's reflection samples finer than it could.
  And the decals' own footprint, still `pixel_spread` times the distance (bug-sv-decal-footprint-past-bounce).
- **Tested:** `sv - a ray cone samples the texture level its footprint covers`, a floor whose levels halve: the camera's level within 0.35 of the formula, and four times as far two levels coarser.
  The test harness now sets `pixel_spread` as a view does, so every path-traced test runs with cones.
- **Not measured:** the time a frame saves; Sponza's megakernel frame is what this issue named, and it is still to be measured.
