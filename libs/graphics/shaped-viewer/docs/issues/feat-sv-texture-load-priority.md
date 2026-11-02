---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/resource_managers.cc
  - file: libs/graphics/shaped-viewer/docs/asset-loading.md
---

# Per-texture load priority is not implemented

Transfers are ordered attributes over geometry over textures, which is the plan's "geometry before textures".
Its other half — base colour before roughness — needs the importer to say which map matters more, and nothing carries that today.
