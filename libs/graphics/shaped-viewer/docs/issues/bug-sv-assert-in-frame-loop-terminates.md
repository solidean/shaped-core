---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - symbol: sv::viewer::~viewer
---

# A failing `CC_ASSERT` inside the frame loop turns into `std::terminate`, not a test failure

nexus reports the assert by throwing, the stack unwinds through `viewer::~viewer`, and `advance_epoch` asserts again on the way out.
A second assert during unwinding is an immediate abort.
So any assert reached from a viewer frame loses its own message behind an `abort()`, which is what once made an empty scene layer expensive to find.
The viewer's destructor should be able to tear down a viewer whose frame did not complete.
`volumetric-furnace-test` reads its readiness before submitting and asserts after, for the same reason.
