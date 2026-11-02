---
created: 2026-10-17
parent: epic-sv-layout-interaction
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/view_state.hh
---

# A lifted view always floats over the whole window, never inside its container

That is deliberate: the caller rebuilds the tree every frame, so the only stable reference box is the window.
But a caller who wants it re-parented into a specific container has no way to ask.
Whether to offer one, and how to name the container stably across rebuilds, is the question.
