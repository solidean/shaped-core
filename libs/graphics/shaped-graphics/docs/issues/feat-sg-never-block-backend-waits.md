---
created: 2026-10-17
tags: [dx12, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.hh
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_context.hh
---

# dx12 and vulkan still wait on the caller's thread under a `never_block` config

sg itself has no blocking spelling any more: frame loops await `epochs_in_flight_completion`, drains await `idle_completion`, and `ctx.execution()` is how a context says it cannot block at all.
The completion-signal seam is callback-shaped (`arm_completion_signal`), and a never-block tier-1 driver on dx12 and vulkan proves the suite gets by without waiting.

**The gap that driver leaves is dx12's and vulkan's own caller-thread waits**, which still wait under a `never_block` config:

- inline ring back-pressure;
- a ring budget change;
- the transient descriptor ring;
- swapchain acquire.

All are backend internals no WebGPU code reaches.
The intended fix is the growth fallback *after* the wait, with a knob to skip the wait and trade VRAM for throughput.

No frame loop in the tree uses `try_advance_epoch` yet.
`sv::viewer`'s synchronous frame API is the one caller that still blocks on sg's completions; that is shaped-viewer's to retire.
The dxc end-to-end window tests block the same way, being synchronous throughout.
