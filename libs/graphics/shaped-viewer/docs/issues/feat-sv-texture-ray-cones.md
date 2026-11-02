---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
---

# Every texture is sampled at mip 0

A footprint along the ray (ray cones) is what picks the level.
On Intel's Sponza (the `sponza` example) it is what the megakernel's frame is left paying for: about 470 ms at 1920x1080 on an RTX 5070 Laptop GPU, against the wavefront's 150 ms.
BC7 compression already took the scene from 7.7 GB, over the device's budget, to 3.1 GB; the wavefront's frame went from over 800 ms to 150 ms with it.
