---
created: 2026-10-20
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/fbx_import.cc
  - file: libs/graphics/shaped-viewer/docs/asset-loading.md
---

# The FBX importer misreads three common material setups

Each is a reading Bistro does not need and a real asset from another pipeline would; none is confirmed on such a file yet.

- **A cutout drops the Opacity constant.**
  A base-color map with any texel below full alpha binds an `opacity` texture, and a bound texture outranks the constant, so a material that is both cut out and partly transparent loses its `Opacity`.
  Game assets that keep gloss in the base color's alpha are cut out at one half wherever it is low.
- **A normal map connected to `Bump` is not bound.**
  3ds Max often connects a tangent-space normal map there; only `NormalMap` is read, and `Bump` raises an issue.
- **Generated tangents follow the first uv set**, even when the normal map samples another one.
