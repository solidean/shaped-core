---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_resources.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
---

# A format-less storage image, `image_2d[.host]`

An `out` image written as `float4` and bound to whatever float format the host has, one compile for every format.
dx12 and MSL have it, vulkan needs `shaderStorageImageWriteWithoutFormat`, and WGSL can never have it, so it is a feature.
Reading one is vulkan's separate `shaderStorageImageReadWithoutFormat`, so a `mut` or read-only `.host` image waits for a shader that wants it.
It is additive: the portable route is an option as the format, one compile per format ([bindings.md](../spec/bindings.md#image-formats)).
