---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - symbol: sv::gpu_resource_manager::_placeholder_texture
---

# The 1x1 placeholder texture is written as four 8-bit channels, whatever the format says

`gpu_resource_manager::_placeholder_texture` creates the stand-in at the record's own format and uploads exactly four bytes into it, one channel each, in rgba order.
Every format the glTF importer produces is that shape.
But `sv::texture_data` carries an arbitrary `sg::pixel_format` and `acquire_texture` is public.
A 1x1 `rgba16_float` subresource is 8 bytes and would get 4; an `r8_unorm` one is 1.

Neither the upload scope nor the backend compares a payload against the region it fills, so nothing catches it in transit.
A `CC_ASSERT` on the block size holds the line until the encode is written per format.
The sRGB curve's channel choice is the same assumption: it encodes components 0..2 and not 3, which is rgba.
