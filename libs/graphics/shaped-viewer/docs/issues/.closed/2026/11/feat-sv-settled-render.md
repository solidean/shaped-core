---
created: 2026-11-11
parent: epic-sv-baking
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/interactive.cc
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/capture.cc
  - symbol: advance_capture
closed: 2026-11-12
resolution: fixed
---

# A settled render: the frame loop inside one call, the image handed back

Today a headless image is `sv::interactive` with an `sr::capture_request`: the caller writes a loop, and the image goes to a file.
A tool that renders as part of its own work wants neither.

## The design, worked out on 2026-11-11 and not yet built

- **A new header `settled.hh`**, with `sv::settled_options` (size, samples, timeout_seconds) and `sv::settled_image` (size, rgba bytes, settled).
- **`sv::render_settled(id, options, author)`**, and an overload taking an `sg::context&`.
  It creates a headless viewer, installs the capture session, runs `for (auto f : v.frames()) author(f)`, and returns once the session is done.
  `author` is the body an interactive loop runs, so one body draws the same image in a window and here; `frame_scope` derives from `frame`, so it takes a `frame&`.
- **The image reaches it through a sink, not a file.**
  `viewer::advance_capture` already decides when an image has settled: payloads landed, every traced layer converged, auto exposure read.
  Its `write` lambda calls `sr::submit_capture_readback` and then encodes to the request's path.
  A private `viewer::install_settled(req, sv::impl::settled_landing*)` stores a landing, a struct holding an optional `sg::bytes_future` and a settled flag, owned by `render_settled`.
  Where a landing is installed, `write` puts the readback there instead of encoding, and the give-up warning names no path.
  `render_settled` is a friend of `sv::viewer`, as `interactive` is.
- **The readback is BGRA8** (`submit_capture_readback` asserts it); the call swaps it to RGBA.
- **One viewer serves one call**: the viewer is single per process for now, which a consumer evaluating bakes on worker threads must serialize around.

## Later

- Return linear radiance, not only the displayed image, once `feat-sv-render-channels` lands.
- An async form, for a caller that must not block a worker.
- The test: a scene rendered by `render_settled` equals the same body's capture through `sv::interactive`, byte for byte.
