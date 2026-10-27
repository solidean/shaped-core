---
created: 2026-10-18
refs:
  - file: libs/graphics/shaped-viewer/tests/wavefront-view-test.cc
  - file: libs/graphics/shaped-viewer/tests/quadric-trace-test.cc
  - test: sv - the wavefront traces the megakernel's quadrics and decals
  - test: sv - the traced silhouette agrees with the CPU reference
  - symbol: sv::pathtrace_routine
---

# The megakernel's quadrics come out wrong on CI's WARP, under the full suite's load

Two quadric tests fail on the Windows Clang job's WARP in about half of its full runs, and never alone.
Both are skipped on software adapters until this is found.

## What the failures show

- **"the wavefront traces the megakernel's quadrics and decals"** compares two captures block by block.
  Two failing runs gave the *same* wavefront values to the last digit, which a deterministic RNG predicts.
  The megakernel's were about twice as bright in the same ceiling blocks, and differed a little between the two runs.
  So the megakernel side is the one that moves.
- **"the traced silhouette agrees with the CPU reference"** traces with `pathtrace_routine` alone: 494 disagreeing pixels against a limit of 231.
  That is the megakernel's quadric intersection disagreeing with the CPU, with no wavefront involved.

## What it is not

- Not the adapter path: both pass on the same runner through `EnumWarpAdapter` and through the Basic Render Driver.
- Not test order: the full binary passes on a local x64 WARP with the failing run's seed.
- Not a plain regression: a rerun of a failing job passed, and so did the two tests alone.

## Where to look

Something that a slow device widens: a quadric hit group still compiling while a stand-in renders is the first suspect.
A pending stand-in restarts the image every frame, but a capture does not count a compile as owed work.
