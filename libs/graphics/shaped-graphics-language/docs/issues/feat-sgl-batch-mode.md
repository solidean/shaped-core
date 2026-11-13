---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/describe_command.cc
  - file: libs/graphics/shaped-shader-library/cmake/sgl_description.py
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md
---

# A batch mode for the sgl tool: one process for many sources

A shader package's build runs `sgl describe` once per SGL source or module (`sgl_description.py`'s `describe`), each in a process of its own.
Every one of those processes parses the prelude, checks it, and checks the module layers its source reaches.

Measured on 2026-11-09, shaped-viewer's tracer pipeline: about 120 ms for a whole `describe`.
About 30 ms of it was process start and exit, and the module layers were a 16 ms check.

A batch mode would describe many sources in one process, paying the prelude and each shared layer once.
That is what the persistent artifacts tried to buy across processes, and the compilation model's artifacts section says why no process keeps one for another.
The shape is open: several sources on one command line, or a list on stdin answered as one JSON document per source.
