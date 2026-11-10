---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc
  - file: libs/graphics/shaped-graphics/backends/metal/tests/metal-vertex-input-test.cc
  - test: sg metal - a draw reads the vertex buffer a dispatch in the same list wrote
  - test: sg metal - a draw sees what the previous draw's fragment shader wrote
  - symbol: metal_command_list::pass_reopens
closed: 2026-11-10
resolution: fixed
---

# The metal encoder-boundary barrier pair is emitted but not proved

`flush_barriers` emits on whichever encoder is open, clamped to the stages that encoder accepts, and every encoder publishes its work as it closes.
So a dependency crossing an encoder boundary is carried by that publish plus the queue wait the next encoder opens with, rather than by the encoder-scoped barrier that cannot reach across one.

**What is not established is that the pair is needed.**
`sg metal - a draw reads the vertex buffer a dispatch in the same list wrote` covers the path and passes with the publish removed.
A four-thread dispatch finishes well before the pass it precedes on an M4.
A test that would catch the ordering needs a dispatch long enough to lose the race, which trades a sharp test for a slow one.
So the pair stands on Apple's documented model rather than on an oracle of ours.

The in-pass case is one step better off: a fragment-stage producer closes and reopens the pass, and `metal_command_list::pass_reopens` makes that observable.
So `sg metal - a draw sees what the previous draw's fragment shader wrote` asserts the mechanism fired rather than only that the pixels came out right.
The ordering itself is still the same race, and still not what a 4x4 draw can prove.

The question is whether a slow, sharp test is worth adding.

## Resolved

Yes, and the test turned out cheap: `emit_quad_main` now spins before it writes, and the test still runs in under 100 ms.
Measured on an M4 on 2026-11-10, ten runs or more per arm:

- With neither half of the pair, it fails on the first run, down to a spin of 16384 rounds; at 2048 it passed 20 of 20.
- The render encoder's queue wait alone orders the draw, 10 of 10.
- The compute encoder's publish alone did not: it held back only compute stages, so the vertex stage read the buffer early.
  Widened to every later stage, it orders the draw alone too, 10 of 10, and that is the fix that landed.

The in-pass fragment-stage case is unchanged: `pass_reopens` asserts the mechanism fires, and its ordering is still the race a 4x4 draw cannot lose.
