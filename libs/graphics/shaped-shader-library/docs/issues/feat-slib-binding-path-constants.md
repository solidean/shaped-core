---
created: 2026-10-15
priority: someday
refs:
  - symbol: sg::bindless_array::for_binding
---

# Maybe: a binding's host path as a generated constant

`sg::bindless_array::for_binding(ctx, group, "scene_textures.textures")` and `declare_array_texture_access` take the path as a string, so a typo is a run-time assert.
The generated group could name each array binding's path, so a caller writes `lumion_shaders::scene_textures::textures_path` instead.
