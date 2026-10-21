---
created: 2026-10-21
refs:
  - test: "sv - the wavefront tracer traces and denoises the viewer's image"
  - file: libs/graphics/shaped-viewer/tests/denoise-view-test.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-21T16:04Z
---

# A capture of the Cornell box lit by an emissive mesh sometimes comes out wrong on WARP

`denoise-view-test.cc`'s "the wavefront tracer traces and denoises the viewer's image" failed on CI's Windows Clang WARP leg on 2026-10-21 (run 39599242298).
Its megakernel capture of the Cornell box, lit by an emissive-mesh lamp alone, averaged 11.33 against the wavefront's 18.31.
So the megakernel went dark by about 38%, with no per-triangle emission involved.
The capture settled in well under its timeout, so it was not an image written short of its samples.

It was first filed under `bug-sv-per-triangle-lamp-flake`, which turned out to be a single firefly in one of thousands of seed sets the compile's timing chose between.
A 38% shortfall of a whole image's mean is no firefly, so this is something else.
One candidate is a frame shaded before the lamp became a light being counted into the capture's mean; that was ruled out for the emitter tests' renders, not for a capture.

## Reproduced locally, 2026-10-21

`uv run dev.py test "sv - the wavefront tracer traces and denoises the viewer's image" --thorough --repeat 25` fails on the WARP leg about once in 5 to 13 runs, on relwithdebinfo, after fae2bfb19.
Every capture that comes out right is the same to the bit from run to run, one image per adapter, so a wrong one is a discrete state rather than noise.
Two shapes of it were seen:
* The raw wavefront capture averaged 12.72 against the megakernel's 18.20; so either tracer can be the one that goes dark.
* The à-trous capture, at 32 samples, came out barely smoother than the raw one (3.02 against 3.28).
  Its image held the wrong geometry: the lamp and the green wall in place, the rest covered by a slanted dark surface cut from the lamp's edge.
  The raw, megakernel and two-sample captures of the same run were right.

Ruled out, each by experiment or by reading:
* A capture reading another's process environment: both dx12 drivers are `exclusive("capture-environment")`, and their invocables run one after another.
* A camera left from an earlier capture: every `sv::interactive` creates its viewer anew, nothing persists per id, and only a double-click starts a glide.
* Owed work: every capture, the wrong ones included, wrote with `pending_work_count() + settling_count()` at 0.
* Denoiser guides outliving a restart: they restart whenever the mean does.

On one adapter the lamp, a 2-triangle mesh, is traced as its placeholder box for a frame or two at the start of each capture, in passing runs as much as failing ones.
A placeholder of a flat mesh is the likeliest source of the slanted surface, so the open question is how a frame of it reaches an image the capture accepts.

Logging masks it: one line printed per traced frame passed 25 runs in a row, and the same lines buffered and printed at the capture's write passed 30.
The next probe should record less, such as the placeholder count alone, kept per capture and checked at the write.
