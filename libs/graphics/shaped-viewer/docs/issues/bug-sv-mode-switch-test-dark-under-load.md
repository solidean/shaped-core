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

2026-11-06: it fails alone too, every run, on dx12 hardware (RTX 5070 laptop), relwithdebinfo.
The phase is `forward`: 375 to 531 of its frames are declined while its pipelines compile, and one frame shown in the middle of that is 0.235 near black.
The failure reproduces at 1828c6adf, 695314073 and every sv commit between them, so none of those caused it; the earlier full-suite pass on 2026-11-06 most likely met warm pipeline caches.
So "under load" is the wrong cue: what it needs is the forward stages compiling while the phase runs.
