---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/asset.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/asset_loader.cc
---

# Both halves of an async load re-fetch and re-parse

Deliberate at this size.
A glTF's JSON is cheap beside its accessors, and sharing the parsed document would mean holding it, and its buffers, twice over.
Worth revisiting if a profile ever disagrees — the fix is a third node the other two depend on.
