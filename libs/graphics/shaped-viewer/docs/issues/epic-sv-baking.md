---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/capture_session.hh
---

# The viewer as a baker: settled renders, any camera model, and named output channels

A consumer bakes a tileable material from a 3D model: an orthographic look down on one tile of it, placed 3 by 3, with the applied materials shaded as the tracer shades them.
Baking through the viewer keeps one material model for renders and bakes, and every improvement to the tracer improves the bakes too.

The viewer stays a renderer: the camera is the caller's, never a baking mode built in.
What it lacks is three general pieces, each its own issue:

1. `feat-sv-settled-render`: a call that renders until the image settles and hands it back, with no frame loop for the caller.
2. `feat-sv-orthographic-camera`: an orthographic projection beside the perspective one, through every path.
3. `feat-sv-render-channels`: outputs beyond radiance, named, each accumulated over the jittered samples like the image.

Done before this epic, 2026-11-11: anisotropic texture filtering at traced hits, and `render_settings::expected_samples_per_pixel`.

Left from `feat-sv-settled-render`, closed 2026-11-12:
- **An async form**, for a caller that must not block a worker thread; the call blocks today.
- **Linear radiance beside the displayed image**, which `feat-sv-render-channels` returns.
