---
created: 2026-11-13
tags: [raster, api]
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/setups/setups.cc
---

# sg has no multisample resolve

A 4-sample target can be drawn into and read sample by sample from a shader, but nothing resolves it to one sample a pixel.
A renderer antialiasing with MSAA wants one, and every backend has it natively.
- dx12: `ResolveSubresource`, or a render pass's resolve target.
- vulkan: a rendering attachment's `resolveImageView`, or `vkCmdResolveImage`.
- metal: a render pass's `resolveTexture`.
- webgpu: a color attachment's `resolveTarget`.

The likely shape is a resolve target beside each color target of `rendering_info`, resolved as the scope ends, which all four take natively.
A standalone `cmd->resolve(from, to)` covers the rest.

The differential corpus's `samples` setup reads each sample through a compute stage instead; with a resolve it gains a setup holding the resolved average across APIs.
