---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/tests/view-accumulation-test.cc
  - test: sv - a view accumulates*
closed: 2026-10-17
resolution: fixed
---

# The accumulation blend is never checked to produce the running mean it claims

The tests pin the *policy*: a camera or scene change restarts the counter, a relayout does not.
Nothing checks that the blend itself produces the running mean it claims to.

Partly closed since this was first written: the integrator is now read back.
`volumetric-furnace-test` and the light tests in `pathtraced-view-test` read traced images, and the closure has `openpbr-bsdf-test`.
None of them separates "the mean of every frame" from "the last frame".

The cheap check is a readback test: trace a converging static view twice at a known constant color and assert the target holds the mean rather than the last frame.
Until that exists, `pathtraced-window-manual-test` is the only real confirmation of the blend.
