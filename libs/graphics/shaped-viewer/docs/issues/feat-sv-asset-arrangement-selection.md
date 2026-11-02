---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/asset_data.hh
  - symbol: sv::asset_data::bounds
---

# A file carrying several arrangements frames as their union

The importer takes no position on which glTF scene is the right one, so every mesh crosses and `asset_data::bounds()` is the world box over all of them.
A caller who framed a camera on a file with two arrangements of the same meshes therefore sees both boxed at once.
That is the correct consequence of importing everything rather than a defect.

What is missing is the other half: a way to say *which* arrangement.
That is a scene-level concern and belongs wherever a scene ends up living rather than as a load option.
Nothing in the repo carries such a file, so nothing would notice today.
