---
created: 2026-10-17
blocked-by: [bug-sv-tangent-frame-generate-ignored]
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/tangent_frames.cc
  - file: libs/graphics/shaped-viewer/docs/asset-loading.md
---

# Generated tangents will not match MikkTSpace

Once we generate any.
Nearly every DCC tool bakes normal maps against MikkTSpace, so an asset only reproduces exactly if the runtime basis matches the baker's.
Per-corner area- and angle-weighted accumulation is close but not identical, and diverges visibly on high-frequency normal maps and mirrored uv shells.
Fixing it means porting MikkTSpace's algorithm into the same mesh-processing helpers.
Blocked on generation itself: there is nothing to diverge yet.
