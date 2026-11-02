---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/layout/layout_tree.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/view_state.hh
---

# Interaction on top of the layout tree

The layout-tree restructure is done.
A frame is flattened into a `render_plan`, `view_renderer` allocates what it names and records its traces, and `viewer_renderer` replays it into one `layout_routine` pass per texture.
The authoring API has landed too: `sv::interactive(id)`, the `view_api` / `window_api` mixins, and non-owning `{frame*, index}` handles.

What is left is the interaction on top of it, in dependency order, as this epic's children.
