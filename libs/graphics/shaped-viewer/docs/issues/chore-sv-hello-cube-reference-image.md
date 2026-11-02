---
created: 2026-10-17
priority: someday
refs:
  - file: libs/graphics/shaped-viewer/examples/hello-cube.cc
  - file: libs/graphics/shaped-viewer/examples/hello-cube.jpg
---

# `shaped-viewer/hello-cube`'s reference image shows one of its six face colours

The example declares a `base_color` per face and spends half its body building them, which is the thing a reader opens it to understand.
In `examples/hello-cube.jpg` the overhead light at emission 14 blows the top face to white.
The third visible face is lit by the sky alone and reads as pale grey rather than purple.
Only the red face survives.

Turning the light down and lowering the elevation a little would put three distinguishable colours in frame.
`uv run dev.py example shaped-viewer/hello-cube --capture` is the loop for it.
Left as it is on purpose — the image shows the viewer working, which is what it is mainly for.
