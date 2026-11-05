---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/interpret.hh
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/context.hh
---

# An agent-friendly frame debugger for sg and SGL, RenderDoc-like

RenderDoc and PIX answer what a frame did, through a GUI; an agent needs the same answers as data it can query.
sg owns every call a frame makes, and SGL owns the shaders' meaning, with an interpreter that runs any entry point exactly (EVAL), so both halves are ours to capture and to replay.

What it should answer, each a child:

- **A capture of a frame**: sg's command lists, the pipelines, bindings and resources each draw and dispatch saw, as a value a test can hold and a tool can read.
- **Pixel history**: for one pixel, every draw that touched it, which entry point ran, with which inputs, and what it wrote or why it was discarded.
- **CPU replay of one invocation**: that pixel's or that thread's SGL entry point run on the interpreter with the captured bindings and stage inputs.
  The interpreter traces every print and value, so a replay steps through the `.sgl` source and states where the GPU's result and the exact one part.
- **An interface for agents**: queries and answers as JSON, over the CLI and an MCP server, never a GUI first.

It meets the robustness profiles and the float ladder where they leave off: a replay shows a NaN's origin, and a GPU result that differs from the exact one under `@float(.ieee)` is a driver bug found.
