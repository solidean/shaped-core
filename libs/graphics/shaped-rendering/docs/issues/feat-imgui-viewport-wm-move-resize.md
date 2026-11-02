---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/imgui_context.cc
  - symbol: sr::window
  - symbol: sr::window_system::poll_events
---

# imgui viewports: tell imgui when the window manager moves or resizes a viewport

`PlatformRequestMove` / `PlatformRequestResize` are unset.
So imgui is not told when the *window manager* rather than imgui moves or resizes a viewport window — a snap, a clamp, a drag by an OS title bar.
imgui then keeps its own idea of the geometry and re-commands it.

It cannot be detected by comparing positions.
`window::position` holds what was last written through, while the platform reports what its event queue last confirmed.
Those differ during any motion for reasons unrelated to the window manager.

It needs the event, `SDL_EVENT_WINDOW_MOVED`, which `sr::input_event` does not model since it carries user input rather than window-state changes.
So the fix is a moved/resized latch on `sr::window`, set in `poll_events` and consumed by the viewport code: a window-API change.
