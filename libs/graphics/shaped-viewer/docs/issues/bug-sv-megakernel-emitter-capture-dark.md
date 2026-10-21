---
created: 2026-10-21
refs:
  - test: "sv - the wavefront tracer traces and denoises the viewer's image"
  - file: libs/graphics/shaped-viewer/tests/denoise-view-test.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-21T16:04Z
---

# A megakernel capture of the Cornell box lit by an emissive mesh once came out 38% dark on WARP

`denoise-view-test.cc`'s "the wavefront tracer traces and denoises the viewer's image" failed on CI's Windows Clang WARP leg on 2026-10-21 (run 39599242298).
Its megakernel capture of the Cornell box, lit by an emissive-mesh lamp alone, averaged 11.33 against the wavefront's 18.31.
So the megakernel went dark by about 38%, with no per-triangle emission involved.
The capture settled in well under its timeout, so it was not an image written short of its samples.

It was first filed under `bug-sv-per-triangle-lamp-flake`, which turned out to be a single firefly in one of thousands of seed sets the compile's timing chose between.
A 38% shortfall of a whole image's mean is no firefly, so this is something else, and not yet reproduced locally.
One candidate is a frame shaded before the lamp became a light being counted into the capture's mean; that was ruled out for the emitter tests' renders, not for a capture.
