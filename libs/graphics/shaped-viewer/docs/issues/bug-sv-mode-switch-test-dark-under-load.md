---
created: 2026-11-02
refs:
  - test: "sv - switching renderers and denoisers never shows black blocks, and a still image settles"
  - file: libs/graphics/shaped-viewer/tests/mode-switch-test.cc
---

# The mode-switch test sees a dark phase when the full suite runs beside it

What is wrong or wanted, and what is already known.

In a full `dev.py test` on dx12 hardware (Windows, 2026-11-02), one phase's darkest frame was 0.168 of the pixels near black at frame 52, against the 0.05 ceiling.
Run alone with `--repeat 3` the test passes every time, so it depends on load: most likely a compile or an upload still pending when the phase starts, which the full suite stretches.
Which phase it was is unknown, since the check dumped the phase name as a pointer; the dump now prints the name, so the next failure says.

Seen again 2026-11-02 in a consumer's ship gate (relwithdebinfo, full suite beside it); the test alone passed right after.

## Log

2026-11-06: it failed alone too, on dx12 hardware (RTX 5070 laptop), relwithdebinfo, and later the same day passed in a full `sv dx12 - hardware` run, so it is intermittent rather than constant.
The phase is `forward`: 375 to 531 of its frames are declined while its pipelines compile, and one frame shown in the middle of that is 0.235 near black.
It failed at 1828c6adf, 695314073 and the sv commits between them, so none of those introduced it.
Load is not the only cue, then; compiling forward stages during the phase may be, which is unproven.

2026-11-12: on metal too, an Apple M4, relwithdebinfo, in two `dev.py ship` checks that ran shaped-viewer-test alone.
The phase was `forward` both times, 0.237 and 0.247 near black, at frames 0 and 13; the run then hung on the failed test's leftover work until the watchdog.
The full shaped-viewer-test passed alone in between, 557 tests, so the order a seed draws is a cue too (seed 3166066474908219465 failed).
