---
created: 2026-10-17
parent: epic-sg-raster-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/raster/vertex_input.hh
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_raster_pipeline.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_raster_pipeline.cc
  - symbol: sg::vertex_attribute
---

# Vertex attributes: go location-based, drop the HLSL semantic from the public API

`vertex_attribute` identifies an input by an **HLSL `semantic` + `semantic_index` string** — the one identity that does not survive a change of shader language.
Every other target matches vertex inputs by a **numeric location**: SPIR-V/Vulkan `layout(location=N)`, WGSL/WebGPU `@location(N)`, Metal `[[attribute(N)]]`.
Vulkan's `VkVertexInputAttributeDescription` is literally `{location, binding, format, offset}`, with no name.
So the backend-neutral identity is a `u32 location`, and `{location, format, offset, slot}` is the union of the Vulkan / WebGPU / Metal models.

**Today vulkan and metal number an input by its attribute's index in `vertex_input_layout::attributes`.**
That makes the shader's `[[vk::location(N)]]` annotations part of the contract — see `vulkan_raster_pipeline.cc`.
Metal's `[[attribute(n)]]` follows the same rule, as the same workaround for the same missing field.

Plan:

- Make `location` the attribute identity, replacing `semantic` / `semantic_index` in `vertex_attribute`.
- Move the HLSL **semantic into `compiled_shader`'s reflected vertex-input signature** as per-input `{location, semantic, semantic_index, format}` — already a deferred field there.
- The **dx12 backend** then resolves `location → semantic` from that signature to fill `D3D12_INPUT_ELEMENT_DESC`, since DX12 is the only backend that needs the string.
  SPIR-V / WGSL / Metal use `location` verbatim and ignore the semantic entirely.
- Optionally keep a semantic **hint** on the layout, resolved to a location at pipeline-build time against the reflected VS input signature — ergonomic sugar for HLSL authors.
  The string is erased before it reaches any backend, so it never appears in the portable path.
