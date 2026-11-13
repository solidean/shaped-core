---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: examples/graphics/web-viewer/web-shell.html
parent: epic-sv-raster-bound
---

# A viewer on a caller's surface gets no input, so the web page's camera cannot move

A viewer created with `viewer_config::surface` brings no window system up, so nothing polls input and every view's camera stays where it was seeded.
graphics/web-viewer's page therefore cannot orbit or fly, though its view asks for `camera_style::fly`.

Wanted: a way to feed the viewer input events from outside, which the page fills from the canvas's pointer, wheel and key events.
The viewer already routes `sr::window_system` events; the open question is whether that seam takes injected events or a second entry point does.
Touch is part of it, since phones are a target.
