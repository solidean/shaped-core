---
created: 2026-11-26
refs:
  - file: examples/graphics/web-viewer/web_viewer.cc
closed: 2026-11-26
resolution: fixed
---

# The web viewer's capture under node fails on a render routine pending for 600 ticks

`uv run dev.py example web-viewer --preset emscripten-webgpu-relwithdebinfo --capture` fails on Windows, seen 2026-11-26.
sg logs "a render routine has been pending for 600 ticks" twice, about 4 s in, which nexus counts as undeclared warnings.
Before 2026-11-26 the run failed earlier, on bcache's no-SQLite warning, so how long this has been there is not known.
Not known: which routine is pending, and whether its shaders fail to compile under node or something it depends on never comes up.

## Log

**2026-11-26: nothing was stuck; the warning measured ticks alone.**
The two routines were `sv::raster_bound_routine` and `sr::bc_compress_routine`, and both come up, given a longer run: their pipelines compile slowly under node.
The headless loop ticked 600 times in 2.5 s, where the threshold meant about 10 s at 60 Hz.
`routine_registry::warn_if_pending_too_long` now waits for 10 s on the steady clock as well, and the example passes.
