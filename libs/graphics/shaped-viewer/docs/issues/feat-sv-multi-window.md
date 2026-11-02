---
created: 2026-10-17
parent: epic-sv-layout-interaction
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/frame.hh
  - symbol: sv::frame::window
---

# A second window, and windows across viewers

Only the default window is presented today; `frame::window(id)` names others but nothing presents them.
A second window within one viewer is only an sv-side step: `sr::window_system` already drives N windows from one poll.

Multi-window compositing across viewers is the larger half.
Multi-view within one window is done; the window system is one-per-process, so this needs shared ownership across viewers.
