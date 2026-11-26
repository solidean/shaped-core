---
created: 2026-11-26
refs:
  - file: examples/graphics/web-viewer/web_viewer.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-26T14:06Z
---

# The web viewer's capture under node fails on a render routine pending for 600 ticks

`uv run dev.py example web-viewer --preset emscripten-webgpu-relwithdebinfo --capture` fails on Windows, seen 2026-11-26.
sg logs "a render routine has been pending for 600 ticks" twice, about 4 s in, which nexus counts as undeclared warnings.
Before 2026-11-26 the run failed earlier, on bcache's no-SQLite warning, so how long this has been there is not known.
Not known: which routine is pending, and whether its shaders fail to compile under node or something it depends on never comes up.
