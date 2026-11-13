---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-viewer/tests/final-render-benchmark.cc
closed: 2026-11-12
resolution: fixed
---

# The megakernel draws horizontal bands on the studio's floor and walls that do not average out

A megakernel render of `.tmp/sample-models/ABeautifulGame.glb` under `scene.studio()` shows horizontal streaks on the studio's floor and lower walls.
They are there at 16 samples per pixel and still at 256, with no noise target and no denoiser, so they are not sampling noise.
Persistent bands on large flat surfaces at grazing angles look like shadow-ray self-intersection (acne); the studio is scaled to the asset, which is small here.
`SV_FINAL_RENDER_PROBE=seq:raw16` and `seq:raw` in the final-render benchmark write such frames.
A noise target makes it more visible, since a smooth wall stops at its minimum frame count and keeps the bands undenoised.

Enlarged, the lines follow lines of constant height on the studio's surfaces: horizontal on the back wall, diagonal on the side wall.
So the structure is in world space rather than in the image's rows, which rules out the random sequences and points at something that varies with a hit's height.

## Resolution

The lines were square rings around the origin: contours of max(|x|, |z|) on the studio floor, seen horizontal ahead and diagonal at the sides.
The floor was one pair of triangles 2 km across, and a hit on a triangle is only as precise as its corners' coordinates.
So every hit's height was off by more than a continuation's offset, and rays struck the floor again.
The "corners" around the asset were the same artifact; a 20 m floor showed neither.
`studio_floor` is now a grid whose lines halve toward the origin, so the cells around the asset are millimeters across with exact corners, and the floor still reaches its extent.
A distance-aware ray offset was tried first and changed nothing, so it was not kept.
