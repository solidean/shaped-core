---
created: 2026-10-17
parent: epic-sv-layout-interaction
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/view_state.hh
  - symbol: sv::impl::view_state::zoom_center
---

# The zoom cannot pan: which binding slides a magnified view?

Ctrl+wheel magnifies at the cursor, which reaches anywhere by zooming out and back in.
There is no way to slide a magnified window at a fixed zoom.
`view_state::zoom_center` is what a drag would write.
The open question is a binding that does not collide with orbit (plain left-drag) or camera pan (middle-drag).
