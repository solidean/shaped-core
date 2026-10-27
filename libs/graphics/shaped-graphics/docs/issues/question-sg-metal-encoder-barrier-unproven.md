---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc
  - file: libs/graphics/shaped-graphics/backends/metal/tests/metal-vertex-input-test.cc
  - test: sg metal - a draw reads the vertex buffer a dispatch in the same list wrote
  - test: sg metal - a draw sees what the previous draw's fragment shader wrote
  - symbol: metal_command_list::pass_reopens
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
