---
created: 2026-11-28
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc
closed: 2026-11-28
resolution: fixed
---

# A direct list writing a texture the context's upload filled in part sometimes fails to close on dx12

A texture array's level 0 uploaded through `ctx.upload` (the async copy queue), then, in the next direct command list, its other levels written:
the raster box-filter mip routine rendering into a single-slice render target of each slice and level, and the compute one writing an image of the levels.
On dx12 that list's `Close` intermittently fails, about one run in four of a suite doing it in several renderers on one context, with no device loss and no debug-layer message logged.
`ID3D12GraphicsCommandList::Close failed` is asserted in `submit_dx12_command_list`, and the reclaim that follows asserts `discard of a buffer this list never touched`.
Uploading level 0 through the same list (`cmd.upload.bytes_to_texture`) instead never failed in ten runs, which points at the cross-queue handoff of a texture whose other subresources the list writes.
Not reduced to a test yet: the next step is a test that uploads one subresource through `ctx.upload` and renders into another of the same texture in the next list, many times, with the debug layer on.

## Cause and fix

A list touching a texture with any transfer in flight tracked every subresource of it in `general`, whatever the access,
so a list rendering the next mip level while level 0 uploaded asked for a render-target write in `general`.
That is D3D12's COMMON layout, which takes no render-target, depth or unordered access, so the barrier was invalid and the list failed to close;
vulkan named `GENERAL` for an attachment its render pass begins in `COLOR_ATTACHMENT_OPTIMAL`, which its validation layer reports.
Intermittent because the choice is read while the list records: only a list recorded before the copy finished hit it.

`sg::impl::transfer_pin` now pins only the subresources the transfers in flight cover, as one box restarted once none is pending,
and only for an access `general` serves, sampling and copies; any other access keeps its own layout, which the GPU still orders after the transfer.
`sg - a list renders into one mip level of a texture while an async upload fills another` reproduced it on dx12 and vulkan every run, and passes 20 runs in a row now.
A list recorded before the transfer is enqueued is still bug-sg-dx12-list-strands-transfer-layout.
