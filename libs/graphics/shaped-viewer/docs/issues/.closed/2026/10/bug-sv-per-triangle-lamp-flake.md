---
created: 2026-10-20
priority: next
refs:
  - test: sv - a per-triangle lamp of two different triangles lights as two constant ones do
  - file: libs/graphics/shaped-viewer/tests/emitter-trace-test.cc
  - symbol: sv::seed_of
closed: 2026-10-21
resolution: fixed
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

Seen again in `dev.py check`'s `release` leg on 2026-10-21, from a consumer's session, on the megakernel.
Block (1, 0) held 2.3049238 and 2.5152862 in channels 0 and 1, nearly the values above, but against expected values of 1.8609288 and 2.175361.
The lamp's image was about 24% brighter than the reference here, not darker; which of the two images left its usual value is not known, since the megakernel's usual reference was not recorded.

## Findings, 2026-10-21

The image is a function of the restart count alone, and the count is what varies from run to run.
Logging `slot.restarts` at each render's 32nd frame over 60 runs, every pair of renders that ended on the same count gave the same block value to the bit (3252, 3490, 3190, 3041 and 3114 each twice).
The per-triangle render ends between about 2900 and 4300 restarts, the constant one between 700 and 1000.
Every frame shaded while a permutation or an emitter is pending restarts the mean, and a debug build compiles for seconds.
So each run draws one of a few thousand seed sets, and 2.3049817 is the image of one of them, which is why it recurs.

The failing image holds a single firefly rather than a bias: pixel (13, 3) on the lamp's edge, its excess triangle 0's emission about 585 times over.
Other failures were fireflies in the reference instead, which reads as the per-triangle image being too dark.
At 512 samples an 8% block tolerance cannot absorb one such sample, so the test fails for whichever seed sets land one.

Ruled out along the way, each by experiment:
* A pending frame inside the final run: no render's final 32 frames held one, failing runs included.
* Concurrency between tests: the test fails alone, under `--jobs 1` too, and no render saw the epoch move between recording and submit.
* Name collisions in the shared material library, and the area CDF, which is area-only and built in the same list ahead of the trace.

Per-frame logging masks the flake, which is what made it look timing-dependent: it changes how many pending frames there are.

Reverting `seed_of` is not the fix.
4d142a489 seeded restarts apart because every moving frame, each a restart, otherwise drew the same sequence.
The fix belongs either in the test, so that its result does not depend on the seed set it draws, or in what advances the restart count.

## Resolution

`restarts` now advances only where a restart for a different image ends a run that drew samples (`restart_for_image`).
Frames that declined to trace and frames restarted only because their last frame traced through something loading no longer count.
Every render of `emitter-trace-test.cc` then ends on 0 to 4 restarts rather than thousands, and 40 debug runs in a row passed.

A capture in `denoise-view-test.cc` was once thought to be in the same state; it is not a firefly but a 38% bias, and is `bug-sv-megakernel-emitter-capture-dark`.
