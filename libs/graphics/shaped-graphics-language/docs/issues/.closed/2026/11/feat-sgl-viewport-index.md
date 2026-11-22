---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/features.hh
closed: 2026-11-22
resolution: fixed
---

# A vertex stage picks the viewport its primitive draws into

`@viewport_index` on a field of the struct a `@vertex fun` returns is `unsupported-yet: the attribute @viewport_index on a struct field`.
It is `@layer`'s sibling (CHK-419), and follows it in every respect:

- At most one field, a `uint`, the rasterizer's and no interpolated value.
- A feature of its own beside `vertex_layer_output`, which WebGPU has on no device: it has no multiple viewports at all.
- dx12 `SV_ViewportArrayIndex`, vulkan `ViewportIndex` (`shaderOutputViewportIndex`), metal `[[viewport_array_index]]`.
- sg needs a way to set more than one viewport for it to mean anything, which is worth checking before the language half lands.
