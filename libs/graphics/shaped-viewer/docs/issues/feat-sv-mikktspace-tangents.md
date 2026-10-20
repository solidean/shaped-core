---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/tangent_frames.cc
  - file: libs/graphics/shaped-viewer/docs/asset-loading.md
---

# Generated tangents do not match MikkTSpace

The FBX importer generates them, for a mesh whose material has a normal map: `impl::tangent_frames_from_uvs`, area-weighted per vertex.
Nearly every DCC tool bakes normal maps against MikkTSpace, so an asset only reproduces exactly if the runtime basis matches the baker's.
Per-corner area- and angle-weighted accumulation is close but not identical, and diverges visibly on high-frequency normal maps and mirrored uv shells.
Fixing it means porting MikkTSpace's algorithm into the same mesh-processing helpers.
