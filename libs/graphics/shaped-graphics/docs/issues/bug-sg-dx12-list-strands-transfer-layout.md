---
created: 2026-10-17
parent: epic-sg-transfer-gaps
tags: [dx12]
refs:
  - test: sg stream - a list recorded before a fresh texture's upload and submitted after it
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc
  - symbol: has_pending_transfer
---

# A list recorded before a transfer and submitted after it strands that transfer's layout on dx12

Reproduced, on WARP and on hardware:

```cpp
auto cmd = ctx->create_command_list();
cmd->ensure_layout(tex, sg::texture_layout::shader_texture);  // entry requirement recorded
ctx->upload.bytes_to_texture(tex, pinned);                     // fixup settles COMMON, job enqueued
ctx->submit_command_list(cc::move(cmd));                       // entry barrier moves it to SHADER_RESOURCE
```

The copy queue then reports `Barrier layout(D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) ... must be in expected layout (D3D12_BARRIER_LAYOUT_COMMON)`.
A D3D12 copy queue cannot run a layout barrier at all, so the copy needs COMMON and the list took it away.
Vulkan survives the same sequence — the semaphore orders it and the layer accepts it — so this is dx12-only today.
`sg stream - a list recorded before a fresh texture's upload and submitted after it` pins the vulkan side and skips dx12 until this is fixed.

**`has_pending_transfer` is what should have caught it, and cannot.**
Both backends' `track_texture_access` force a texture to `general` while any transfer stamp is unreached.
But it is consulted while *recording*, and no transfer was pending then — the enqueue comes afterwards.
That is not a timing accident.
sg's happens-before model is the order of calls on the *context*, and recording is none of those events, so a value read there answers a question the model does not pose.
[concepts/barriers.md](../concepts/barriers.md#what-orders-what-the-calls-on-the-context) is the contract.

**The fix is the one [perf-sg-direction-specific-async-layout](perf-sg-direction-specific-async-layout.md) already names.**
Submit the fixup from the transfer actor, immediately before the job it belongs to, so a list submitting in between cannot get underneath it.
Clamping the entry layout at finalize instead does not work — a vulkan copy command names its layout literally, captured at record, so the body and the entry barrier would disagree.
Until then the guard is worth neither trusting nor deleting: rewrite it in terms of context-call order, or remove it with the fix.
