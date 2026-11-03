---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-viewer/tests/quadric-raster-test.cc
---

# A capture of a view holding only quadrics settles at once on a blank image

A capture through `sv::interactive` of a view whose scene holds a quadric set and nothing else writes an all-black image within a second.
That is the megakernel's view as much as the raster path's, and the background's own color is missing too, so nothing was presented at all.
The same scene with one triangle mesh added, even one out of view, captures as expected.

Measured on 2026-11-02 with `quadric-raster-test.cc`, whose out-of-view triangle is the workaround this issue names.
Hypothesis: the capture's settle test counts traced views or dispatched traces through something only a mesh sets.
A scene without a mesh would then have nothing to wait for, and settle on its first frame.
