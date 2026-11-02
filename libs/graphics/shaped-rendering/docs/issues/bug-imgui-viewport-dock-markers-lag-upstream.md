---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
tags: [upstream]
refs:
  - file: libs/graphics/shaped-rendering/docs/imgui.md
  - test: sr::imgui_routine - a non-zero display pos shifts what lands on the target
---

# imgui viewports: dock-target markers lag a dragged viewport window (upstream)

**Upstream bug, not ours.**
While a detached viewport window is dragged over another, the dock-target markers drawn inside it lag the window.
They snap back into place the moment it stops moving.

Reproduced with **stock imgui**: upstream `example_win32_directx12` built from the tag we vendor (`v1.92.8-docking`, `b61e5634`), with upstream's own win32 + dx12 backends and none of our code.
So nothing in `sr` causes it and nothing in `sr` can fix it.

Matches [ocornut/imgui#7664](https://github.com/ocornut/imgui/issues/7664) ("docking markers move with window").
That one is reported against glfw + vulkan on X11: two unrelated backend stacks, same symptom.
Unresolved upstream, no maintainer response.
What would close this here is an imgui bump that carries a fix.

Ruled out along the way, each by measurement rather than argument, so nobody repeats it:
- the input translation to desktop space;
- the coordinates handed to the routine (window position equals `ImDrawData::DisplayPos` at submit time, sampled from `GetWindowRect`);
- the routine's own handling of a non-zero `DisplayPos`, now pinned by `sr::imgui_routine - a non-zero display pos shifts what lands on the target`;
- the order of viewport update against the main window's present.
