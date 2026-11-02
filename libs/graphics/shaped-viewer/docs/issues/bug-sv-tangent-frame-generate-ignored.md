---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/asset_loader.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/tangent_frames.cc
  - symbol: sv::tangent_frame_options
  - symbol: sv::frame_generation
---

# `tangent_frame_options::generate` is accepted and ignored

Only `prefer_file` is implemented: a file's own normals and tangents cross (a glTF `TANGENT` among them).
`smooth` / `crease` do nothing, silently, which is worse than rejecting them.
`smooth` is `per_vertex` after a position weld; `crease` is `per_corner`, welded then split by `crease_angle`.
Both want the mesh-processing helpers that eventually move down to `tg::mesh`, so they are written as free functions over spans when they land.

This is also what a mesh with uvs but no tangents needs: `openpbr-spheres` produces an analytic frame per vertex, and the FBX importer generates one from the uvs for a normal-mapped mesh.
Any other such mesh gets the geometric fallback.
