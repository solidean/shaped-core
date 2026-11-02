---
created: 2026-10-17
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/CMakeLists.txt
---

# dx12 links the Windows SDK's D3D12 libraries with no presence or version check

dx12 links `d3d12 dxgi dxguid` straight off the default lib path, with no explicit SDK presence or version check.
vulkan is the contrast: it gates on `find_package(Vulkan)` and links `Vulkan::Vulkan`.
Its device floor is 1.3 plus descriptor_buffer and robustness2, refused by name at creation.
