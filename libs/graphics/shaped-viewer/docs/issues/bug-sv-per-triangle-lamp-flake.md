---
created: 2026-10-20
priority: next
refs:
  - test: sv - a per-triangle lamp of two different triangles lights as two constant ones do
  - file: libs/graphics/shaped-viewer/tests/emitter-trace-test.cc
  - symbol: sv::seed_of
---

# A per-triangle lamp of two different triangles sometimes lights its box about 10% too dark

`emitter-trace-test.cc`'s two-triangle lamp test fails now and then on hardware, about once in 7 to 30 runs, and passes on its own most of the time.
It compares the box lit by one per-triangle emitter against the same box lit by two constant emitters, one per triangle.

The failure is bimodal rather than noise.
Two wavefront failures held block (1, 0)'s red channel at 1.877 and 1.874 against a reference of exactly 2.0968504 both times, about 10% low.
One cold-compile run failed on the megakernel instead, block (1, 1)'s green and blue 13% and 19% low.
The reference image is the same to the bit every time, so it is the per-triangle lamp's image that lands in a second, darker state.
Red is what the brighter triangle (24, 18, 9) gives most, so the dark state looks like samples reading the other triangle's emission, or the mean of the two.

It began with the merge of #233's wavefront into the lighting branch: the branch alone passed 60 runs of 60, and the merged build failed on run 7 of 30.
The merge changed the accumulation seed to hash the slot's restart count, which varies with compile timing, so the suspect is a seed sequence the lamp's sampling handles badly.
Not tested yet: swapping `seed_of` back to `accum_frame + 1` and repeating the test.
Not re-measured since #240's review fixes, which changed when a scaled frame counts a restart and made a frame with a lamp still compiling restart.

Repeat with `uv run dev.py test "a per-triangle lamp of two different" --repeat 60`, which stops at the first failure and keeps its logs.

Seen twice in `dev.py check`'s `debug-nopch-clang` leg on 2026-10-20, on main, with the same value both times: block (1, 0) channel 0 holds 2.3049817 and channel 1 2.5153303.
So the dark state looks like one fixed wrong answer rather than noise, and both times the next run of the test step passed.

Seen again in `dev.py check`'s `release` leg on 2026-10-21, from a solidean session, on the megakernel.
Block (1, 0) held 2.3049238 and 2.5152862 in channels 0 and 1, nearly the values above, but against expected values of 1.8609288 and 2.175361.
The lamp's image was about 24% brighter than the reference here, not darker; which of the two images left its usual value is not known, since the megakernel's usual reference was not recorded.

## A second test in the same dark state

`denoise-view-test.cc`'s "the wavefront tracer traces and denoises the viewer's image" failed on CI's Windows Clang WARP leg on 2026-10-21 (run 39599242298).
Its megakernel capture of the Cornell box, lit by an emissive-mesh lamp alone, averaged 11.33 against the wavefront's 18.31.
So here the megakernel is the side that went dark, by about 38%, with no per-triangle emission involved — any emissive-mesh lamp may reach the state.
The capture settled in well under its timeout, so it was not an image written short of its samples.
