---
created: 2026-10-17
parent: epic-sg-presentation-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/present/swapchain.hh
  - symbol: sg::swapchain::is_hdr_enabled
---

# Deeper HDR presentation

Metadata and tone-mapping beyond the colorspace set.

Including whether the request was *granted*.
`enable_hdr` is best-effort on dx12 and vulkan, and webgpu's canvas has no HDR arm yet.
`is_hdr_enabled()` reports what was asked for, so nothing tells a caller which colorspace it actually got.
