---
created: 2026-11-24
priority: normal
tags: [sg, vulkan, hlsl]
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_context.create.cc
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_context.hh
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/binding/impl/portability.cc
  - symbol: sg::feature::readwrite_norm16_image_formats
---

# sg's vulkan backend neither checks nor enables reading a storage image that states no format

A read-write storage image that states no format reads through SPIR-V's `StorageImageReadWithoutFormat` capability.
That is what HLSL compiles to: an `RWTexture2D<float4>` without a `[[vk::image_format(...)]]` annotation is an `OpTypeImage` of format `Unknown`.
Vulkan allows such a read only with `shaderStorageImageReadWithoutFormat` enabled,
or, from Vulkan 1.3 (`VK_KHR_format_feature_flags2`), on a format whose features carry `VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT`.

sg's vulkan backend asks neither.
It does not enable `shaderStorageImageReadWithoutFormat` at device creation, does not report it through any `sg::feature`,
and checks no per-format `STORAGE_READ_WITHOUT_FORMAT` bit for the view bound to a format-less read-write image.
So a pipeline of such an image is accepted by sg and is invalid usage on vulkan, which validation reports and a driver may get wrong.
The same holds for the write side (`shaderStorageImageWriteWithoutFormat`, `STORAGE_WRITE_WITHOUT_FORMAT`).

This predates the 16-bit normalized formats and affects every format-less read-write image, whatever its format.
SGL's own SPIR-V states every image's format, so it is HLSL-sourced shaders (and any other SPIR-V of `Unknown` format) that reach it.

Julius' rule (2026-11-24): what may hold only on some backends is behind a proper feature flag.
So this is either an `sg::feature` (format-less read-write images, granted where the device feature or the per-format bits allow)
or a device requirement the vulkan backend refuses a device without.
Whether dx12 needs the same (a typed UAV load outside the formats its per-format query lists) is part of the decision;
for the norm16 formats sg already judges it per format (`readwrite_norm16_image_formats`).

## Wanted

- Decided: a feature or a device requirement, and how the per-format bits of Vulkan 1.3 enter it.
- The vulkan backend enables `shaderStorageImageReadWithoutFormat` (and the write one) where the device has it, as it enables
  `shaderStorageImageExtendedFormats`.
- sg refuses a binding of a format-less read-write image without it, with a message naming the feature, and judges a bound view's format
  against `STORAGE_READ_WITHOUT_FORMAT` where only the per-format route allows it.
- A device test: a format-less `RWTexture2D` read and written on vulkan, and the refusal on a device (or a feature set) without it.

## Accepted meanwhile: an HLSL write-only image of the norm16 formats is read-write

A binding group layout reflected from HLSL makes every `RWTexture` read-write, since HLSL has no write-only image.
So a shader that only writes an image of `r16_unorm`, `r16_snorm`, `rg16_unorm`, `rg16_snorm`, `rgba16_unorm` or `rgba16_snorm`
is refused on a device without `sg::feature::readwrite_norm16_image_formats` (every webgpu and metal device, and a dx12 or vulkan device
whose per-format query says no), though the shader would have run there.
Julius accepted this (2026-11-24): the refusal's message says so and that the shader itself has to change
(`impl::find_unsupported_view` in portability.cc); an SGL shader declares the image `out` and is not refused.
