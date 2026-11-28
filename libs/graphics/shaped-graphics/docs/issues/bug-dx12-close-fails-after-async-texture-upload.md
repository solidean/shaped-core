---
created: 2026-11-28
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc
---

# A direct list writing a texture the context's upload filled in part sometimes fails to close on dx12

A texture array's level 0 uploaded through `ctx.upload` (the async copy queue), then, in the next direct command list, its other levels written:
the raster box-filter mip routine rendering into a single-slice render target of each slice and level, and the compute one writing an image of the levels.
On dx12 that list's `Close` intermittently fails, about one run in four of a suite doing it in several renderers on one context, with no device loss and no debug-layer message logged.
`ID3D12GraphicsCommandList::Close failed` is asserted in `submit_dx12_command_list`, and the reclaim that follows asserts `discard of a buffer this list never touched`.
Uploading level 0 through the same list (`cmd.upload.bytes_to_texture`) instead never failed in ten runs, which points at the cross-queue handoff of a texture whose other subresources the list writes.
Not reduced to a test yet: the next step is a test that uploads one subresource through `ctx.upload` and renders into another of the same texture in the next list, many times, with the debug layer on.
