---
created: 2026-11-09
tags: [webgpu, raster, perf]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/raster_bound_routine.cc
  - file: examples/graphics/web-viewer/web_viewer.cc
---

# The bound raster mode shades the OpenPBR grid at 40 frames a second on an AMD iGPU

Measured 2026-11-09 with graphics/web-viewer in Firefox on the development laptop's AMD integrated GPU, the example's per-frame timing line:

| Variant | Frames a second | Waiting for the GPU per frame |
|---|---|---|
| full scene, 1280x720 | 40 | 24 ms |
| no scene, 1280x720 | 430 | 2.2 ms |
| full scene, 640 wide | 328 once settled | 2.25 ms |
| full scene, 320 wide | 335 | 2.2 ms |

The CPU spends 0.7 ms a frame, and Firefox's completion latency is about 2 ms, so the 24 ms is the GPU's shading, which scales with the pixels drawn.
The 640-wide run also spent its first ~10 s at 40 frames a second, which may be pipeline compiles in Firefox's GPU process holding up the queue; unverified.

The same page in Firefox on the laptop's RTX 5070 runs at 336 frames a second, near that 2 ms floor, so only a weak GPU shows the cost.
Firefox picks the integrated GPU although sg requests the high-performance adapter, which is [Bugzilla 2016063](https://bugzilla.mozilla.org/show_bug.cgi?id=2016063).
Setting Firefox to "High performance" in Windows' per-app graphics settings is what moved it.

Likely causes, unmeasured: no depth prepass, and draws in arbitrary order, so the spheres and the floor shade overlapping pixels with full OpenPBR more than once.
Candidates: a depth prepass as the bindless raster mode has, front-to-back order, and a cheaper shading path for weak GPUs.

## 2026-11-09, the modern hall

The glTF Research Scenes' modern hall, its 13 emitters drawn as rect lights, ran Firefox on the RTX 5070 at 40 frames a second, 24 ms a frame.
Natively the same frame took 6.15 to 6.7 ms against 0.4 ms without the lights: each rect was a 4x4 grid of full OpenPBR evaluations, 208 a pixel.
In Firefox the cost per light was about four times native, and above about 8 lights a frame sat at a 40 frames a second ceiling, unexplained.

- A depth prepass now runs first; it saved 8% natively on that view.
- `raster.rect_light` evaluates a rect in two closure evaluations: 0.79 ms natively, and Firefox at 327 frames a second, its 2.2 ms floor.

The sphere grid on the AMD iGPU, which this issue opened with, is not re-measured.
